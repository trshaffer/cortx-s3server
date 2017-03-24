/*
 * COPYRIGHT 2016 SEAGATE LLC
 *
 * THIS DRAWING/DOCUMENT, ITS SPECIFICATIONS, AND THE DATA CONTAINED
 * HEREIN, ARE THE EXCLUSIVE PROPERTY OF SEAGATE TECHNOLOGY
 * LIMITED, ISSUED IN STRICT CONFIDENCE AND SHALL NOT, WITHOUT
 * THE PRIOR WRITTEN PERMISSION OF SEAGATE TECHNOLOGY LIMITED,
 * BE REPRODUCED, COPIED, OR DISCLOSED TO A THIRD PARTY, OR
 * USED FOR ANY PURPOSE WHATSOEVER, OR STORED IN A RETRIEVAL SYSTEM
 * EXCEPT AS ALLOWED BY THE TERMS OF SEAGATE LICENSES AND AGREEMENTS.
 *
 * YOU SHOULD HAVE RECEIVED A COPY OF SEAGATE'S LICENSE ALONG WITH
 * THIS RELEASE. IF NOT PLEASE CONTACT A SEAGATE REPRESENTATIVE
 * http://www.seagate.com/contact
 *
 * Original author:  Kaustubh Deorukhkar   <kaustubh.deorukhkar@seagate.com>
 * Author         :  Rajesh Nambiar        <rajesh.nambiar@seagate.com>
 * Original creation date: 13-Jan-2016
 */

#include <string>

#include "s3_error_codes.h"
#include "s3_get_multipart_bucket_action.h"
#include "s3_iem.h"
#include "s3_log.h"
#include "s3_object_metadata.h"
#include "s3_option.h"

S3GetMultipartBucketAction::S3GetMultipartBucketAction(
    std::shared_ptr<S3RequestObject> req)
    : S3Action(req),
      last_key(""),
      return_list_size(0),
      fetch_successful(false),
      last_uploadid("") {
  s3_log(S3_LOG_DEBUG, "Constructor\n");

  s3_clovis_api = std::make_shared<ConcreteClovisAPI>();
  request_marker_key = request->get_query_string_value("key-marker");
  if (!request_marker_key.empty()) {
    multipart_object_list.set_request_marker_key(request_marker_key);
  }
  s3_log(S3_LOG_DEBUG, "request_marker_key = %s\n", request_marker_key.c_str());

  last_key = request_marker_key;  // as requested by user

  request_marker_uploadid = request->get_query_string_value("upload-id-marker");
  multipart_object_list.set_request_marker_uploadid(request_marker_uploadid);
  s3_log(S3_LOG_DEBUG, "request_marker_uploadid = %s\n",
         request_marker_uploadid.c_str());
  last_uploadid = request_marker_uploadid;

  setup_steps();

  multipart_object_list.set_bucket_name(request->get_bucket_name());
  request_prefix = request->get_query_string_value("prefix");
  multipart_object_list.set_request_prefix(request_prefix);
  s3_log(S3_LOG_DEBUG, "prefix = %s\n", request_prefix.c_str());

  request_delimiter = request->get_query_string_value("delimiter");
  multipart_object_list.set_request_delimiter(request_delimiter);
  s3_log(S3_LOG_DEBUG, "delimiter = %s\n", request_delimiter.c_str());

  std::string maxuploads = request->get_query_string_value("max-uploads");
  if (maxuploads.empty()) {
    max_uploads = 1000;
    multipart_object_list.set_max_uploads("1000");
  } else {
    max_uploads = std::stoul(maxuploads);
    multipart_object_list.set_max_uploads(maxuploads);
  }
  s3_log(S3_LOG_DEBUG, "max-uploads = %s\n", maxuploads.c_str());
  // TODO request param validations
}

void S3GetMultipartBucketAction::setup_steps() {
  s3_log(S3_LOG_DEBUG, "Setting up the action\n");
  add_task(std::bind(&S3GetMultipartBucketAction::fetch_bucket_info, this));
  add_task(std::bind(&S3GetMultipartBucketAction::get_next_objects, this));
  add_task(
      std::bind(&S3GetMultipartBucketAction::send_response_to_s3_client, this));
  // ...
}

void S3GetMultipartBucketAction::fetch_bucket_info() {
  s3_log(S3_LOG_DEBUG, "Entering\n");
  bucket_metadata = std::make_shared<S3BucketMetadata>(request);
  bucket_metadata->load(
      std::bind(&S3GetMultipartBucketAction::next, this),
      std::bind(&S3GetMultipartBucketAction::send_response_to_s3_client, this));
  s3_log(S3_LOG_DEBUG, "Exiting\n");
}

void S3GetMultipartBucketAction::get_next_objects() {
  s3_log(S3_LOG_DEBUG, "Entering\n");
  if (check_shutdown_and_rollback()) {
    s3_log(S3_LOG_DEBUG, "Exiting\n");
    return;
  }
  s3_log(S3_LOG_DEBUG, "Fetching next set of multipart uploads listing\n");
  if (bucket_metadata->get_state() == S3BucketMetadataState::present) {
    struct m0_uint128 empty_indx_oid = {0ULL, 0ULL};
    struct m0_uint128 indx_oid = bucket_metadata->get_multipart_index_oid();
    if (m0_uint128_cmp(&indx_oid, &empty_indx_oid) != 0) {
      size_t count = S3Option::get_instance()->get_clovis_idx_fetch_count();
      clovis_kv_reader =
          std::make_shared<S3ClovisKVSReader>(request, s3_clovis_api);
      clovis_kv_reader->next_keyval(
          bucket_metadata->get_multipart_index_oid(), last_key, count,
          std::bind(&S3GetMultipartBucketAction::get_next_objects_successful,
                    this),
          std::bind(&S3GetMultipartBucketAction::get_next_objects_failed,
                    this));
    } else {
      fetch_successful = true;
      send_response_to_s3_client();
    }
  } else {
    s3_log(S3_LOG_ERROR, "Bucket not found\n");
    send_response_to_s3_client();
  }
}

void S3GetMultipartBucketAction::get_next_objects_successful() {
  s3_log(S3_LOG_DEBUG, "Entering\n");
  if (check_shutdown_and_rollback()) {
    s3_log(S3_LOG_DEBUG, "Exiting\n");
    return;
  }
  s3_log(S3_LOG_DEBUG, "Found multipart uploads listing\n");
  struct m0_uint128 indx_oid = bucket_metadata->get_multipart_index_oid();
  bool atleast_one_json_error = false;
  bool skip_marker_key = true;
  auto& kvps = clovis_kv_reader->get_key_values();
  size_t length = kvps.size();
  for (auto& kv : kvps) {
    s3_log(S3_LOG_DEBUG, "Read Object = %s\n", kv.first.c_str());
    auto object = std::make_shared<S3ObjectMetadata>(request, true);
    size_t delimiter_pos = std::string::npos;

    if (object->from_json(kv.second.second) != 0) {
      atleast_one_json_error = true;
      s3_log(S3_LOG_ERROR,
             "Json Parsing failed. Index = %lu %lu, Key = %s, Value = %s\n",
             indx_oid.u_hi, indx_oid.u_lo, kv.first.c_str(),
             kv.second.second.c_str());
    }

    if (skip_marker_key && !request_marker_uploadid.empty() &&
        !request_marker_key.empty()) {
      skip_marker_key = false;
      std::string upload_id = object->get_upload_id();
      if (!request_marker_key.compare(kv.first) &&
          !upload_id.compare(request_marker_uploadid)) {
        --length;
        continue;
      }
    }

    if (request_prefix.empty() && request_delimiter.empty()) {
      return_list_size++;
      multipart_object_list.add_object(object);
    } else if (!request_prefix.empty() && request_delimiter.empty()) {
      // Filter out by prefix
      if (kv.first.find(request_prefix) == 0) {
        return_list_size++;
        multipart_object_list.add_object(object);
      }
    } else if (request_prefix.empty() && !request_delimiter.empty()) {
      delimiter_pos = kv.first.find(request_delimiter);
      if (delimiter_pos == std::string::npos) {
        return_list_size++;
        multipart_object_list.add_object(object);
      } else {
        // Roll up
        s3_log(S3_LOG_DEBUG, "Delimiter %s found at pos %zu in string %s\n",
               request_delimiter.c_str(), delimiter_pos, kv.first.c_str());
        multipart_object_list.add_common_prefix(
            kv.first.substr(0, delimiter_pos + 1));
      }
    } else {
      // both prefix and delimiter are not empty
      bool prefix_match = (kv.first.find(request_prefix) == 0) ? true : false;
      if (prefix_match) {
        delimiter_pos =
            kv.first.find(request_delimiter, request_prefix.length());
        if (delimiter_pos == std::string::npos) {
          return_list_size++;
          multipart_object_list.add_object(object);
        } else {
          s3_log(S3_LOG_DEBUG, "Delimiter %s found at pos %zu in string %s\n",
                 request_delimiter.c_str(), delimiter_pos, kv.first.c_str());
          multipart_object_list.add_common_prefix(
              kv.first.substr(0, delimiter_pos + 1));
        }
      }  // else no prefix match, filter it out
    }

    if (--length == 0 || return_list_size == max_uploads) {
      // this is the last element returned or we reached limit requested
      last_key = kv.first;
      last_uploadid = object->get_upload_id();
      break;
    }
  }
  if (atleast_one_json_error) {
    s3_iem(LOG_ERR, S3_IEM_METADATA_CORRUPTED, S3_IEM_METADATA_CORRUPTED_STR,
           S3_IEM_METADATA_CORRUPTED_JSON);
  }
  // We ask for more if there is any.
  size_t count_we_requested =
      S3Option::get_instance()->get_clovis_idx_fetch_count();

  if ((return_list_size == max_uploads) || (kvps.size() < count_we_requested)) {
    // Go ahead and respond.
    if (return_list_size == max_uploads) {
      multipart_object_list.set_response_is_truncated(true);
    }
    multipart_object_list.set_next_marker_key(last_key);
    multipart_object_list.set_next_marker_uploadid(last_uploadid);
    fetch_successful = true;
    send_response_to_s3_client();
  } else {
    get_next_objects();
  }
}

void S3GetMultipartBucketAction::get_next_objects_failed() {
  if (clovis_kv_reader->get_state() == S3ClovisKVSReaderOpState::missing) {
    s3_log(S3_LOG_DEBUG, "No more multipart uploads listing\n");
    fetch_successful = true;  // With no entries.
  } else {
    s3_log(S3_LOG_DEBUG, "Failed to fetch multipart listing\n");
    fetch_successful = false;
  }
  send_response_to_s3_client();
}

void S3GetMultipartBucketAction::send_response_to_s3_client() {
  s3_log(S3_LOG_DEBUG, "Entering\n");

  if (reject_if_shutting_down()) {
    // Send response with 'Service Unavailable' code.
    s3_log(S3_LOG_DEBUG, "sending 'Service Unavailable' response...\n");
    S3Error error("ServiceUnavailable", request->get_request_id(),
                  request->get_object_uri());
    std::string& response_xml = error.to_xml();
    request->set_out_header_value("Content-Type", "application/xml");
    request->set_out_header_value("Content-Length",
                                  std::to_string(response_xml.length()));
    request->set_out_header_value("Retry-After", "1");

    request->send_response(error.get_http_status_code(), response_xml);
  } else if (fetch_successful) {
    std::string& response_xml = multipart_object_list.get_multiupload_xml();

    request->set_out_header_value("Content-Length",
                                  std::to_string(response_xml.length()));
    request->set_out_header_value("Content-Type", "application/xml");
    s3_log(S3_LOG_DEBUG, "Object list response_xml = %s\n",
           response_xml.c_str());

    request->send_response(S3HttpSuccess200, response_xml);
  } else if (bucket_metadata->get_state() == S3BucketMetadataState::missing) {
    // Invalid Bucket Name
    S3Error error("NoSuchBucket", request->get_request_id(),
                  request->get_object_uri());
    std::string& response_xml = error.to_xml();
    request->set_out_header_value("Content-Type", "application/xml");
    request->set_out_header_value("Content-Length",
                                  std::to_string(response_xml.length()));

    request->send_response(error.get_http_status_code(), response_xml);
  } else {
    S3Error error("InternalError", request->get_request_id(),
                  request->get_bucket_name());
    std::string& response_xml = error.to_xml();
    request->set_out_header_value("Content-Type", "application/xml");
    request->set_out_header_value("Content-Length",
                                  std::to_string(response_xml.length()));

    request->send_response(error.get_http_status_code(), response_xml);
  }
  done();
  i_am_done();  // self delete
  s3_log(S3_LOG_DEBUG, "Exiting\n");
}
