#include "object/s3_store.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <aws/core/client/AWSClient.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>

namespace pulpfs {
namespace {

std::mutex aws_sdk_mutex;
uint64_t aws_sdk_ref_count = 0;
Aws::SDKOptions aws_sdk_options;

Error ErrorFromS3Failure(const std::string& operation, const std::string& key, const auto& error) {
    const std::string message = operation + " failed for object " + key + ": " +
        error.GetExceptionName() + ": " + error.GetMessage();
    if (error.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) {
        return MakeError(NotFound{.path = key}, message);
    }
    return MakeError(Unavailable{}, message);
}

std::string RangeHeader(const GetObjectRequest& request) {
    if (!request.offset.has_value()) {
        return {};
    }

    std::ostringstream range;
    range << "bytes=" << *request.offset << "-";
    if (request.length.has_value() && *request.length > 0) {
        range << (*request.offset + *request.length - 1);
    }
    return range.str();
}

}  // namespace

AwsSdkRuntime::AwsSdkRuntime() {
    std::lock_guard lock(aws_sdk_mutex);
    if (aws_sdk_ref_count == 0) {
        Aws::InitAPI(aws_sdk_options);
    }
    ++aws_sdk_ref_count;
}

AwsSdkRuntime::~AwsSdkRuntime() {
    std::lock_guard lock(aws_sdk_mutex);
    --aws_sdk_ref_count;
    if (aws_sdk_ref_count == 0) {
        Aws::ShutdownAPI(aws_sdk_options);
    }
}

S3Store::S3Store(S3StoreConfig config) : config_(std::move(config)) {
    Aws::Client::ClientConfiguration client_config;
    client_config.endpointOverride = config_.endpoint;
    client_config.region = config_.region;
    client_config.scheme = config_.use_https ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
    client_config.verifySSL = config_.verify_ssl;
    client_config.maxConnections = config_.max_connections;
    client_config.connectTimeoutMs = config_.connect_timeout_ms;
    client_config.requestTimeoutMs = config_.request_timeout_ms;

    client_ = std::make_unique<Aws::S3::S3Client>(
        client_config, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        !config_.path_style
    );
}

Result<PutObjectResult> S3Store::PutObject(PutObjectRequest request) {
    Aws::S3::Model::PutObjectRequest s3_request;
    s3_request.SetBucket(request.bucket);
    s3_request.SetKey(request.key);
    s3_request.SetContentType(request.content_type);
    s3_request.SetContentLength(static_cast<long>(request.body.size()));

    auto body = Aws::MakeShared<Aws::StringStream>("PulpfsS3PutObject");
    body->write(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    body->seekg(0);
    s3_request.SetBody(body);

    auto outcome = client_->PutObject(s3_request);
    if (!outcome.IsSuccess()) {
        return std::unexpected(ErrorFromS3Failure("put object", request.key, outcome.GetError()));
    }

    PutObjectResult result;
    result.etag = outcome.GetResult().GetETag();
    result.size = request.body.size();
    return result;
}

Result<GetObjectResult> S3Store::GetObject(GetObjectRequest request) {
    if (request.length.has_value() && !request.offset.has_value()) {
        return std::unexpected(MakeError(InvalidArgument{}, "object range length requires offset"));
    }

    Aws::S3::Model::GetObjectRequest s3_request;
    s3_request.SetBucket(request.bucket);
    s3_request.SetKey(request.key);

    const std::string range = RangeHeader(request);
    if (!range.empty()) {
        s3_request.SetRange(range);
    }

    auto outcome = client_->GetObject(s3_request);
    if (!outcome.IsSuccess()) {
        return std::unexpected(ErrorFromS3Failure("get object", request.key, outcome.GetError()));
    }

    auto& s3_result = outcome.GetResult();
    auto& stream = s3_result.GetBody();

    GetObjectResult result;
    result.body.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    result.etag = s3_result.GetETag();
    result.content_length =
        static_cast<uint64_t>(std::max<int64_t>(s3_result.GetContentLength(), 0));
    return result;
}

Result<void> S3Store::DeleteObject(DeleteObjectRequest request) {
    Aws::S3::Model::DeleteObjectRequest s3_request;
    s3_request.SetBucket(request.bucket);
    s3_request.SetKey(request.key);

    auto outcome = client_->DeleteObject(s3_request);
    if (!outcome.IsSuccess()) {
        return std::unexpected(
            ErrorFromS3Failure("delete object", request.key, outcome.GetError())
        );
    }
    return {};
}

Result<ListObjectsResult> S3Store::ListObjects(ListObjectsRequest request) {
    Aws::S3::Model::ListObjectsV2Request s3_request;
    s3_request.SetBucket(request.bucket);
    s3_request.SetPrefix(request.prefix);
    s3_request.SetMaxKeys(1000);
    if (!request.continuation_token.empty()) {
        s3_request.SetContinuationToken(request.continuation_token);
    }

    auto outcome = client_->ListObjectsV2(s3_request);
    if (!outcome.IsSuccess()) {
        return std::unexpected(
            ErrorFromS3Failure("list objects", request.prefix, outcome.GetError())
        );
    }

    ListObjectsResult result;
    const auto& s3_result = outcome.GetResult();
    for (const auto& object : s3_result.GetContents()) {
        ObjectInfo info;
        info.key = object.GetKey();
        info.size = static_cast<uint64_t>(std::max<long long>(object.GetSize(), 0));
        info.last_modified_ms = object.GetLastModified().Millis();
        result.objects.push_back(std::move(info));
    }
    result.next_continuation_token = s3_result.GetNextContinuationToken();
    return result;
}

}  // namespace pulpfs
