/* AWS V4 Signature implementation
 *
 * This file contains the modularized source code for accepting a given HTTP
 * request as ngx_http_request_t and modifiying it to introduce the
 * Authorization header in compliance with the AWS V4 spec. The IAM access
 * key and the signing key (not to be confused with the secret key) along
 * with it's scope are taken as inputs.
 *
 * The actual nginx module binding code is not present in this file. This file
 * is meant to serve as an "AWS Signing SDK for nginx".
 *
 * Maintainer/contributor rules
 *
 * (1) All functions here need to be static and inline.
 * (2) Every function must have it's own set of unit tests.
 * (3) The code must be written in a thread-safe manner. This is usually not
 *     a problem with standard nginx functions. However, care must be taken
 *     when using very old C functions such as strtok, gmtime, etc. etc.
 *     Always use the _r variants of such functions
 * (4) All heap allocation must be done using ngx_pool_t instead of malloc
 */

#ifndef __NGX_AWS_FUNCTIONS_INTERNAL__H__
#define __NGX_AWS_FUNCTIONS_INTERNAL__H__

#include <time.h>
#include <ngx_times.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "aws_vars.h"
#include "aws_crypto_helper.h"

typedef ngx_keyval_t header_pair_t;

struct AwsCanonicalRequestDetails {
    ngx_str_t *canon_request;
    ngx_str_t *signed_header_names;
    ngx_array_t *header_list; // list of header_pair_t
};

struct AwsCanonicalHeaderDetails {
    ngx_str_t *canon_header_str;
    ngx_str_t *signed_header_names;
    ngx_array_t *header_list; // list of header_pair_t
};

struct AwsSignedRequestDetails {
    const ngx_str_t *signature;
    const ngx_str_t *signed_header_names;
    ngx_array_t *header_list; // list of header_pair_t
};

static inline char *__CHAR_PTR_U(u_char *ptr) { return (char *) ptr; }

static inline const char *__CONST_CHAR_PTR_U(const u_char *ptr) { return (const char *) ptr; }

static inline const ngx_str_t *ngx_aws_auth__compute_request_time(ngx_pool_t *pool, const time_t *timep) {
    ngx_str_t *const retval = ngx_palloc(pool, sizeof(ngx_str_t));
    retval->data = ngx_palloc(pool, AMZ_DATE_MAX_LEN);
    //struct tm *tm_p = ngx_palloc(pool, sizeof(struct tm));
    //gmtime_r(timep, tm_p);
    //retval->len = strftime(__CHAR_PTR_U(retval->data), AMZ_DATE_MAX_LEN - 1, "%Y%m%dT%H%M%SZ", tm_p);
    retval->len = ngx_snprintf(retval->data, AMZ_DATE_MAX_LEN, "%s", "20180917T174000Z") - retval->data;
    return retval;
}

static inline int ngx_aws_auth__cmp_hnames(const void *one, const void *two) {
    header_pair_t *first, *second;
    int ret;
    first = (header_pair_t *) one;
    second = (header_pair_t *) two;
    ret = ngx_strncmp(first->key.data, second->key.data, ngx_min(first->key.len, second->key.len));
    if (ret != 0) {
        return ret;
    } else {
        return (first->key.len - second->key.len);
    }
}

static inline const ngx_str_t *ngx_aws_auth__canonize_query_string(
        ngx_pool_t *pool,
        const ngx_http_request_t *req) {
    u_char *p, *ampersand, *equal, *last;
    size_t i, len;
    ngx_str_t *retval = ngx_palloc(pool, sizeof(ngx_str_t));

    header_pair_t *qs_arg;
    ngx_array_t *query_string_args = ngx_array_create(pool, 0, sizeof(header_pair_t));

    if (req->args.len == 0) {
        return &EMPTY_STRING;
    }

    p = req->args.data;
    last = p + req->args.len;

    for ( /* void */ ; p < last; p++) {
        qs_arg = ngx_array_push(query_string_args);

        ampersand = ngx_strlchr(p, last, '&');
        if (ampersand == NULL) {
            ampersand = last;
        }

        equal = ngx_strlchr(p, last, '=');
        if ((equal == NULL) || (equal > ampersand)) {
            equal = ampersand;
        }

        len = equal - p;
        qs_arg->key.data = ngx_palloc(pool, len * 3);
        qs_arg->key.len = (u_char *) ngx_escape_uri(qs_arg->key.data, p, len, NGX_ESCAPE_ARGS) - qs_arg->key.data;


        len = ampersand - equal;
        if (len > 0) {
            qs_arg->value.data = ngx_palloc(pool, len * 3);
            qs_arg->value.len = (u_char *) ngx_escape_uri(qs_arg->value.data, equal + 1, len - 1, NGX_ESCAPE_ARGS) -
                                qs_arg->value.data;
        } else {
            qs_arg->value = EMPTY_STRING;
        }

        p = ampersand;
    }

    ngx_qsort(query_string_args->elts, (size_t) query_string_args->nelts,
              sizeof(header_pair_t), ngx_aws_auth__cmp_hnames);

    retval->data = ngx_palloc(pool, req->args.len * 3 + query_string_args->nelts * 2);
    retval->len = 0;

    for (i = 0; i < query_string_args->nelts; i++) {
        qs_arg = &((header_pair_t *) query_string_args->elts)[i];

        ngx_memcpy(retval->data + retval->len, qs_arg->key.data, qs_arg->key.len);
        retval->len += qs_arg->key.len;

        *(retval->data + retval->len) = '=';
        retval->len++;

        ngx_memcpy(retval->data + retval->len, qs_arg->value.data, qs_arg->value.len);
        retval->len += qs_arg->value.len;

        *(retval->data + retval->len) = '&';
        retval->len++;
    }
    retval->len--;
    return retval;
}

static inline struct AwsCanonicalHeaderDetails ngx_aws_auth__canonize_headers(
        ngx_pool_t *pool,
        const ngx_http_request_t *req,
        const ngx_str_t *s3_bucket, const ngx_str_t *amz_date,
        const ngx_str_t *content_hash,
        const ngx_str_t *s3_endpoint) {
    size_t header_names_size = 1, header_nameval_size = 1;
    size_t i, used;
    u_char *buf_progress;
    struct AwsCanonicalHeaderDetails retval;

    ngx_array_t *settable_header_array = ngx_array_create(pool, 3, sizeof(header_pair_t));
    header_pair_t *header_ptr;

    header_ptr = ngx_array_push(settable_header_array);
    header_ptr->key = AMZ_HASH_HEADER;
    header_ptr->value = *content_hash;

    header_ptr = ngx_array_push(settable_header_array);
    header_ptr->key = AMZ_DATE_HEADER;
    header_ptr->value = *amz_date;

    header_ptr = ngx_array_push(settable_header_array);
    header_ptr->key = HOST_HEADER;
    header_ptr->value.len = s3_bucket->len + s3_endpoint->len + 1;
    header_ptr->value.data = ngx_palloc(pool, header_ptr->value.len);
    header_ptr->value.len =
            ngx_snprintf(header_ptr->value.data, header_ptr->value.len, "%V.%V", s3_bucket, s3_endpoint) -
            header_ptr->value.data;

    ngx_qsort(settable_header_array->elts, (size_t) settable_header_array->nelts,
              sizeof(header_pair_t), ngx_aws_auth__cmp_hnames);
    retval.header_list = settable_header_array;

    for (i = 0; i < settable_header_array->nelts; i++) {
        header_names_size += ((header_pair_t *) settable_header_array->elts)[i].key.len + 1;
        header_nameval_size += ((header_pair_t *) settable_header_array->elts)[i].key.len + 1;
        header_nameval_size += ((header_pair_t *) settable_header_array->elts)[i].value.len + 2;
    }

    /* make canonical headers string */
    retval.canon_header_str = ngx_palloc(pool, sizeof(ngx_str_t));
    retval.canon_header_str->data = ngx_palloc(pool, header_nameval_size);

    for (i = 0, used = 0, buf_progress = retval.canon_header_str->data;
         i < settable_header_array->nelts;
         i++, used = buf_progress - retval.canon_header_str->data) {
        buf_progress = ngx_snprintf(buf_progress, header_nameval_size - used, "%V:%V\n",
                                    &((header_pair_t *) settable_header_array->elts)[i].key,
                                    &((header_pair_t *) settable_header_array->elts)[i].value);
    }
    retval.canon_header_str->len = used;

    /* make signed headers */
    retval.signed_header_names = ngx_palloc(pool, sizeof(ngx_str_t));
    retval.signed_header_names->data = ngx_palloc(pool, header_names_size);

    for (i = 0, used = 0, buf_progress = retval.signed_header_names->data;
         i < settable_header_array->nelts;
         i++, used = buf_progress - retval.signed_header_names->data) {
        buf_progress = ngx_snprintf(buf_progress, header_names_size - used, "%V;",
                                    &((header_pair_t *) settable_header_array->elts)[i].key);
    }
    used--;
    retval.signed_header_names->len = used;
    retval.signed_header_names->data[used] = 0;

    return retval;
}

static inline const ngx_str_t *ngx_aws_auth__request_body_hash(
        ngx_pool_t *pool,
        const ngx_http_request_t *req, ngx_http_data_input_ctx_t *ctx) {
    if (ctx == NULL || ctx->body_sha256 == NULL) {
        return &EMPTY_STRING_SHA256;
    }

    return ngx_aws_auth__hash_sha256(pool, ctx->body_sha256);
}

// AWS wants a peculiar kind of URI-encoding: they want RFC 3986, except that
// slashes shouldn't be encoded...
// this function is a light wrapper around ngx_escape_uri that does exactly that
// modifies the source in place if it needs to be escaped
// see http://docs.aws.amazon.com/general/latest/gr/sigv4-create-canonical-request.html
static inline void ngx_aws_auth__escape_uri(ngx_pool_t *pool, ngx_str_t *src) {
    u_char *escaped_data;
    u_int escaped_data_len, escaped_data_with_slashes_len, i, j;
    uintptr_t escaped_count, slashes_count = 0;

    // first, we need to know how many characters need to be escaped
    escaped_count = ngx_escape_uri(NULL, src->data, src->len, NGX_ESCAPE_URI_COMPONENT);
    // except slashes should not be escaped...
    if (escaped_count > 0) {
        for (i = 0; i < src->len; i++) {
            if (src->data[i] == '/') {
                slashes_count++;
            }
        }
    }

    if (escaped_count == slashes_count) {
        // nothing to do! nothing but slashes escaped (if even that)
        return;
    }

    // each escaped character is replaced by 3 characters
    escaped_data_len = src->len + escaped_count * 2;
    escaped_data = ngx_palloc(pool, escaped_data_len);
    ngx_escape_uri(escaped_data, src->data, src->len, NGX_ESCAPE_URI_COMPONENT);

    // now we need to go back and re-replace each occurrence of %2F with a slash
    escaped_data_with_slashes_len = src->len + (escaped_count - slashes_count) * 2;
    if (slashes_count > 0) {
        for (i = 0, j = 0; i < escaped_data_with_slashes_len; i++) {
            if (j < escaped_data_len - 2 && strncmp((char *) (escaped_data + j), "%2F", 3) == 0) {
                escaped_data[i] = '/';
                j += 3;
            } else {
                escaped_data[i] = escaped_data[j];
                j++;
            }
        }

        src->len = escaped_data_with_slashes_len;
    } else {
        // no slashes
        src->len = escaped_data_len;
    }

    src->data = escaped_data;
}

static inline const ngx_str_t *ngx_aws_auth__canon_url(ngx_pool_t *pool, const ngx_http_request_t *req) {
    ngx_str_t *retval;
    const u_char *req_uri_data;
    u_int req_uri_len;

    if (req->args.len == 0) {
        req_uri_data = req->uri.data;
        req_uri_len = req->uri.len;
    } else {
        req_uri_data = req->uri_start;
        req_uri_len = req->args_start - req->uri_start - 1;
    }

    // we need to copy that data to not modify the request for other modules
    retval = ngx_palloc(pool, sizeof(ngx_str_t));
    retval->data = ngx_palloc(pool, req_uri_len);
    ngx_memcpy(retval->data, req_uri_data, req_uri_len);
    retval->len = req_uri_len;

    // then URI-encode it per RFC 3986
    ngx_aws_auth__escape_uri(pool, retval);
    return retval;
}

static inline struct AwsCanonicalRequestDetails ngx_aws_auth__make_canonical_request(
        ngx_pool_t *pool,
        const ngx_http_request_t *req,
        ngx_http_data_input_ctx_t *ctx,
        const ngx_str_t *s3_bucket_name, const ngx_str_t *amz_date, const ngx_str_t *s3_endpoint) {
    struct AwsCanonicalRequestDetails retval;

    // canonize query string
    const ngx_str_t *canon_qs = ngx_aws_auth__canonize_query_string(pool, req);

    // compute request body hash
    const ngx_str_t *request_body_hash = ngx_aws_auth__request_body_hash(pool, req, ctx);

    const struct AwsCanonicalHeaderDetails canon_headers =
            ngx_aws_auth__canonize_headers(pool, req, s3_bucket_name, amz_date, request_body_hash, s3_endpoint);
    retval.signed_header_names = canon_headers.signed_header_names;

    const ngx_str_t *http_method = &(req->method_name);
    const ngx_str_t *url = ngx_aws_auth__canon_url(pool, req);

    retval.canon_request = ngx_palloc(pool, sizeof(ngx_str_t));
    retval.canon_request->len = 5 + http_method->len + url->len + canon_qs->len + canon_headers.canon_header_str->len +
                                canon_headers.signed_header_names->len + request_body_hash->len;
    retval.canon_request->data = ngx_palloc(pool, retval.canon_request->len);

    retval.canon_request->len =
            ngx_snprintf(retval.canon_request->data, retval.canon_request->len, "%V\n%V\n%V\n%V\n%V\n%V",
                         http_method, url, canon_qs, canon_headers.canon_header_str,
                         canon_headers.signed_header_names, request_body_hash) - retval.canon_request->data;
    retval.header_list = canon_headers.header_list;

    return retval;
}

static inline const ngx_str_t *ngx_aws_auth__string_to_sign(
        ngx_pool_t *pool,
        const ngx_str_t *key_scope, const ngx_str_t *date, const ngx_str_t *canon_request_hash) {
    ngx_str_t *retval = ngx_palloc(pool, sizeof(ngx_str_t));

    retval->len = STRING_TO_SIGN_LENGTH;
    retval->data = ngx_palloc(pool, retval->len);
    retval->len = ngx_snprintf(retval->data, retval->len, "AWS4-HMAC-SHA256\n%V\n%V\n%V", date, key_scope,
                               canon_request_hash) - retval->data;

    return retval;
}

static inline const ngx_str_t *ngx_aws_auth__make_auth_token(
        ngx_pool_t *pool,
        const ngx_str_t *signature, const ngx_str_t *signed_header_names,
        const ngx_str_t *access_key_id, const ngx_str_t *key_scope) {

    const char FMT_STRING[] = "AWS4-HMAC-SHA256 Credential=%V/%V,SignedHeaders=%V,Signature=%V";
    ngx_str_t *authz;

    authz = ngx_palloc(pool, sizeof(ngx_str_t));
    authz->len = access_key_id->len + key_scope->len + signed_header_names->len
                 + signature->len + sizeof(FMT_STRING);
    authz->data = ngx_palloc(pool, authz->len);
    authz->len = ngx_snprintf(authz->data, authz->len, FMT_STRING, access_key_id, key_scope, signed_header_names,
                              signature) - authz->data;
    return authz;
}

static inline ngx_str_t *ngx_aws_auth__get_date(ngx_pool_t *pool, const ngx_str_t *datetime) {
    ngx_str_t *ret_val = ngx_palloc(pool, sizeof(ngx_str_t));
    if (datetime->len >= 8) {
        ret_val->len = 8;
    } else {
        ret_val->len = datetime->len;
    }
    ret_val->data = ngx_palloc(pool, ret_val->len);
    ret_val->len = ngx_snprintf(ret_val->data, ret_val->len, "%V", datetime) - ret_val->data;

    return ret_val;
}

static inline ngx_str_t *
ngx_aws_auth__get_signing(
        ngx_pool_t *pool, const ngx_http_request_t *req, const ngx_str_t *key_scope_with_date,
        const ngx_str_t *signing_key) {
    ngx_str_t *step_binary = NULL;
    char *pch, *prev_pch;
    int prev_position = 0;
    int position = 0;
    prev_pch = (char *) key_scope_with_date->data;
    pch = ngx_strchr(key_scope_with_date->data, '/');
    int i = 0;
    while (pch != NULL) {
        position = (u_char *) pch - key_scope_with_date->data;

        if (i == 0) {
            step_binary = ngx_aws_auth__sign_hmac_sha256_data_only(pool, (u_char *) prev_pch, position - prev_position,
                                                                   signing_key);
        } else {
            step_binary = ngx_aws_auth__sign_hmac_sha256_data_only(pool, (u_char *) prev_pch, position - prev_position,
                                                                   step_binary);
        }

        prev_position = position + 1;
        prev_pch = pch + 1;
        pch = strchr(pch + 1, '/');
        ++i;

        if (pch > (char *) (key_scope_with_date->data + key_scope_with_date->len)) {
            break;
        }
    }

    if (step_binary != NULL && prev_pch != NULL) {
        step_binary = ngx_aws_auth__sign_hmac_sha256_data_only(pool, (u_char *) prev_pch,
                                                               key_scope_with_date->len - prev_position, step_binary);
    }

    return step_binary;
}

static inline struct AwsSignedRequestDetails ngx_aws_auth__compute_signature(
        ngx_pool_t *pool, ngx_http_request_t *req,
        ngx_http_data_input_ctx_t *ctx,
        const ngx_str_t *signing_key,
        const ngx_str_t *key_scope_with_date,
        const ngx_str_t *s3_bucket_name,
        const ngx_str_t *s3_endpoint,
        const ngx_str_t *date_time) {
    struct AwsSignedRequestDetails retval;

    if (key_scope_with_date == NULL || key_scope_with_date->len == 0) {
        ngx_log_error(NGX_LOG_ERR, req->connection->log, 0, "aws_sign module: no key_scope defined");
        retval.signature = NULL;
        return retval;
    }

    const struct AwsCanonicalRequestDetails canon_request =
            ngx_aws_auth__make_canonical_request(pool, req, ctx, s3_bucket_name, date_time, s3_endpoint);

    const ngx_str_t *canon_request_hash = ngx_aws_auth__hash_sha256(pool, canon_request.canon_request);
    const ngx_str_t *string_to_sign = ngx_aws_auth__string_to_sign(pool, key_scope_with_date, date_time,
                                                                   canon_request_hash);
    const ngx_str_t *kSigningBinary = ngx_aws_auth__get_signing(pool, req, key_scope_with_date, signing_key);
    const ngx_str_t *signature = ngx_aws_auth__sign_sha256_hex(pool, string_to_sign, kSigningBinary);

    retval.signature = signature;
    retval.signed_header_names = canon_request.signed_header_names;
    retval.header_list = canon_request.header_list;
    return retval;
}

// list of header_pair_t
static inline const ngx_array_t *ngx_aws_auth__sign_v4(
        ngx_pool_t *pool, ngx_http_request_t *req,
        ngx_http_data_input_ctx_t *ctx,
        const ngx_str_t *access_key_id,
        const ngx_str_t *signing_key,
        const ngx_str_t *key_scope,
        const ngx_str_t *s3_bucket_name,
        const ngx_str_t *s3_endpoint) {
    // get dates
    const ngx_str_t *date_time = ngx_aws_auth__compute_request_time(pool, &req->start_sec);
    const ngx_str_t *date = ngx_aws_auth__get_date(pool, date_time);

    // get string to sign
    ngx_str_t *key_scope_with_date = ngx_palloc(pool, sizeof(ngx_str_t));
    key_scope_with_date->len = key_scope->len + date->len + 2;
    key_scope_with_date->data = ngx_palloc(pool, key_scope_with_date->len);
    key_scope_with_date->len =
            ngx_snprintf(key_scope_with_date->data, key_scope_with_date->len, "%V/%V\0", date, key_scope) -
            key_scope_with_date->data;

    const struct AwsSignedRequestDetails signature_details = ngx_aws_auth__compute_signature(pool, req, ctx,
                                                                                             signing_key,
                                                                                             key_scope_with_date,
                                                                                             s3_bucket_name,
                                                                                             s3_endpoint, date_time);
    if (signature_details.signature == NULL) {
        return NULL;
    }

    const ngx_str_t *auth_header_value = ngx_aws_auth__make_auth_token(pool, signature_details.signature,
                                                                       signature_details.signed_header_names,
                                                                       access_key_id, key_scope_with_date);

    header_pair_t *header_ptr;
    header_ptr = ngx_array_push(signature_details.header_list);
    header_ptr->key = AUTHZ_HEADER;
    header_ptr->value = *auth_header_value;

    return signature_details.header_list;
}

#endif