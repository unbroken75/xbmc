/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "WebServer.h"

#ifdef HAS_WEB_SERVER
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

#if defined(TARGET_POSIX)
#include <pthread.h>
#endif

#include "filesystem/File.h"
#include "network/httprequesthandler/HTTPRequestHandlerUtils.h"
#include "network/httprequesthandler/IHTTPRequestHandler.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "ServiceBroker.h"
#include "threads/SingleLock.h"
#include "URL.h"
#include "Util.h"
#include "utils/Base64.h"
#include "utils/log.h"
#include "utils/Mime.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "XBDateTime.h"

#ifdef TARGET_WINDOWS_DESKTOP
#ifndef _DEBUG
#pragma comment(lib, "libmicrohttpd.lib")
#else  // _DEBUG
#pragma comment(lib, "libmicrohttpd_d.lib")
#endif // _DEBUG
#endif // TARGET_WINDOWS_DESKTOP

#define MAX_POST_BUFFER_SIZE 2048

#define PAGE_FILE_NOT_FOUND "<html><head><title>File not found</title></head><body>File not found</body></html>"
#define NOT_SUPPORTED       "<html><head><title>Not Supported</title></head><body>The method you are trying to use is not supported by this server</body></html>"

#define HEADER_VALUE_NO_CACHE "no-cache"

#define HEADER_NEWLINE        "\r\n"

typedef struct {
  std::shared_ptr<XFILE::CFile> file;
  CHttpRanges ranges;
  size_t rangeCountTotal;
  std::string boundary;
  std::string boundaryWithHeader;
  std::string boundaryEnd;
  bool boundaryWritten;
  std::string contentType;
  uint64_t writePosition;
} HttpFileDownloadContext;

CWebServer::CWebServer()
  : m_port(0),
    m_daemon_ip6(nullptr),
    m_daemon_ip4(nullptr),
    m_running(false),
    m_thread_stacksize(0),
    m_authenticationRequired(false),
    m_authenticationUsername("kodi"),
    m_authenticationPassword(""),
    m_key(),
    m_cert()
{
#if defined(TARGET_DARWIN)
  void *stack_addr;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_getstack(&attr, &stack_addr, &m_thread_stacksize);
  pthread_attr_destroy(&attr);
  // double the stack size under darwin, not sure why yet
  // but it stoped crashing using Kodi iOS remote -> play video.
  // non-darwin will pass a value of zero which means 'system default'
  m_thread_stacksize *= 2;
  CLog::Log(LOGDEBUG, "CWebServer: increasing thread stack to %zu", m_thread_stacksize);
#endif
}

static MHD_Response* create_response(size_t size, void* data, int free, int copy)
{
#if (MHD_VERSION >= 0x00094001)
  MHD_ResponseMemoryMode mode = MHD_RESPMEM_PERSISTENT;
  if (copy)
    mode = MHD_RESPMEM_MUST_COPY;
  else if (free)
    mode = MHD_RESPMEM_MUST_FREE;
  return MHD_create_response_from_buffer(size, data, mode);
#else
  return MHD_create_response_from_data(size, data, free, copy);
#endif
}

int CWebServer::AskForAuthentication(const HTTPRequest& request) const
{
  struct MHD_Response *response = create_response(0, nullptr, MHD_NO, MHD_NO);
  if (!response)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: unable to create HTTP Unauthorized response", m_port);
    return MHD_NO;
  }

  int ret = AddHeader(response, MHD_HTTP_HEADER_CONNECTION, "close");
  if (!ret)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: unable to prepare HTTP Unauthorized response", m_port);
    MHD_destroy_response(response);
    return MHD_NO;
  }

  LogResponse(request, MHD_HTTP_UNAUTHORIZED);

  ret = MHD_queue_basic_auth_fail_response(request.connection, "XBMC", response);
  MHD_destroy_response(response);

  return ret;
}

bool CWebServer::IsAuthenticated(const HTTPRequest& request) const
{
  CSingleLock lock(m_critSection);

  if (!m_authenticationRequired)
    return true;

  // try to retrieve username and password for basic authentication
  char* password = nullptr;
  char* username = MHD_basic_auth_get_username_password(request.connection, &password);

  if (username == nullptr || password == nullptr)
    return false;

  // compare the received username and password
  bool authenticated = m_authenticationUsername.compare(username) == 0 &&
                       m_authenticationPassword.compare(password) == 0;

  free(username);
  free(password);

  return authenticated;
}

#if (MHD_VERSION >= 0x00040001)
int CWebServer::AnswerToConnection(void *cls, struct MHD_Connection *connection,
                      const char *url, const char *method,
                      const char *version, const char *upload_data,
                      size_t *upload_data_size, void **con_cls)
#else
int CWebServer::AnswerToConnection(void *cls, struct MHD_Connection *connection,
                      const char *url, const char *method,
                      const char *version, const char *upload_data,
                      unsigned int *upload_data_size, void **con_cls)
#endif
{
  if (cls == nullptr || con_cls == nullptr || *con_cls == nullptr)
  {
    CLog::Log(LOGERROR, "CWebServer[unknown]: invalid request received");
    return MHD_NO;
  }

  CWebServer *webServer = reinterpret_cast<CWebServer*>(cls);
  if (webServer == nullptr)
  {
    CLog::Log(LOGERROR, "CWebServer[unknown]: invalid request received");
    return MHD_NO;
  }

  ConnectionHandler* connectionHandler = reinterpret_cast<ConnectionHandler*>(*con_cls);
  HTTPMethod methodType = GetHTTPMethod(method);
  HTTPRequest request = { webServer, connection, connectionHandler->fullUri, url, methodType, version };

  if (connectionHandler->isNew)
    webServer->LogRequest(request);

  return webServer->HandlePartialRequest(connection, connectionHandler, request, upload_data, upload_data_size, con_cls);
}

int CWebServer::HandlePartialRequest(struct MHD_Connection *connection, ConnectionHandler* connectionHandler, const HTTPRequest& request, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
  std::unique_ptr<ConnectionHandler> conHandler(connectionHandler);

  // remember if the request was new
  bool isNewRequest = conHandler->isNew;
  // because now it isn't anymore
  conHandler->isNew = false;

  // reset con_cls and set it if still necessary
  *con_cls = nullptr;

  if (!IsAuthenticated(request)) 
    return AskForAuthentication(request);

  // check if this is the first call to AnswerToConnection for this request
  if (isNewRequest)
  {
    // look for a IHTTPRequestHandler which can take care of the current request
    auto handler = FindRequestHandler(request);
    if (handler != nullptr)
    {
      // if we got a GET request we need to check if it should be cached
      if (request.method == GET)
      {
        if (handler->CanBeCached())
        {
          bool cacheable = IsRequestCacheable(request);

          CDateTime lastModified;
          if (handler->GetLastModifiedDate(lastModified) && lastModified.IsValid())
          {
            // handle If-Modified-Since or If-Unmodified-Since
            std::string ifModifiedSince = HTTPRequestHandlerUtils::GetRequestHeaderValue(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_IF_MODIFIED_SINCE);
            std::string ifUnmodifiedSince = HTTPRequestHandlerUtils::GetRequestHeaderValue(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_IF_UNMODIFIED_SINCE);

            CDateTime ifModifiedSinceDate;
            CDateTime ifUnmodifiedSinceDate;
            // handle If-Modified-Since (but only if the response is cacheable)
            if (cacheable &&
              ifModifiedSinceDate.SetFromRFC1123DateTime(ifModifiedSince) &&
              lastModified.GetAsUTCDateTime() <= ifModifiedSinceDate)
            {
              struct MHD_Response *response = create_response(0, nullptr, MHD_NO, MHD_NO);
              if (response == nullptr)
              {
                CLog::Log(LOGERROR, "CWebServer[%hu]: failed to create a HTTP 304 response", m_port);
                return MHD_NO;
              }

              return FinalizeRequest(handler, MHD_HTTP_NOT_MODIFIED, response);
            }
            // handle If-Unmodified-Since
            else if (ifUnmodifiedSinceDate.SetFromRFC1123DateTime(ifUnmodifiedSince) &&
              lastModified.GetAsUTCDateTime() > ifUnmodifiedSinceDate)
              return SendErrorResponse(request, MHD_HTTP_PRECONDITION_FAILED, request.method);
          }

          // pass the requested ranges on to the request handler
          handler->SetRequestRanged(IsRequestRanged(request, lastModified));
        }
      }
      // if we got a POST request we need to take care of the POST data
      else if (request.method == POST)
      {
        // as ownership of the connection handler is passed to libmicrohttpd we must not destroy it
        SetupPostDataProcessing(request, conHandler.get(), handler, con_cls);

        // as ownership of the connection handler has been passed to libmicrohttpd we must not destroy it
        conHandler.release();

        return MHD_YES;
      }

      return HandleRequest(handler);
    }
  }
  // this is a subsequent call to AnswerToConnection for this request
  else
  {
    // again we need to take special care of the POST data
    if (request.method == POST)
    {
      // process additional / remaining POST data
      if (ProcessPostData(request, conHandler.get(), upload_data, upload_data_size, con_cls))
      {
        // as ownership of the connection handler has been passed to libmicrohttpd we must not destroy it
        conHandler.release();

        return MHD_YES;
      }

      // finalize POST data processing
      FinalizePostDataProcessing(conHandler.get());

      // check if something went wrong while handling the POST data
      if (conHandler->errorStatus != MHD_HTTP_OK)
        return SendErrorResponse(request, conHandler->errorStatus, request.method);

      // we have handled all POST data so it's time to invoke the IHTTPRequestHandler
      return HandleRequest(conHandler->requestHandler);
    }

    // it's unusual to get more than one call to AnswerToConnection for none-POST requests, but let's handle it anyway
    auto requestHandler = FindRequestHandler(request);
    if (requestHandler != nullptr)
      return HandleRequest(requestHandler);
  }

  CLog::Log(LOGERROR, "CWebServer[%hu]: couldn't find any request handler for %s", m_port, request.pathUrl.c_str());
  return SendErrorResponse(request, MHD_HTTP_NOT_FOUND, request.method);
}

#if (MHD_VERSION >= 0x00040001)
int CWebServer::HandlePostField(void *cls, enum MHD_ValueKind kind, const char *key,
                                const char *filename, const char *content_type,
                                const char *transfer_encoding, const char *data, uint64_t off,
                                size_t size)
#else
int CWebServer::HandlePostField(void *cls, enum MHD_ValueKind kind, const char *key,
                                const char *filename, const char *content_type,
                                const char *transfer_encoding, const char *data, uint64_t off,
                                unsigned int size)
#endif
{
  ConnectionHandler *conHandler = (ConnectionHandler *)cls;

  if (conHandler == nullptr || conHandler->requestHandler == nullptr ||
      key == nullptr || data == nullptr || size == 0)
  {
    CLog::Log(LOGERROR, "CWebServer: unable to handle HTTP POST field");
    return MHD_NO;
  }

  conHandler->requestHandler->AddPostField(key, std::string(data, size));
  return MHD_YES;
}

int CWebServer::HandleRequest(const std::shared_ptr<IHTTPRequestHandler>& handler)
{
  if (handler == nullptr)
    return MHD_NO;

  HTTPRequest request = handler->GetRequest();
  int ret = handler->HandleRequest();
  if (ret == MHD_NO)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: failed to handle HTTP request for %s", m_port, request.pathUrl.c_str());
    return SendErrorResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, request.method);
  }

  const HTTPResponseDetails &responseDetails = handler->GetResponseDetails();
  struct MHD_Response *response = nullptr;
  switch (responseDetails.type)
  {
    case HTTPNone:
      CLog::Log(LOGERROR, "CWebServer[%hu]: HTTP request handler didn't process %s", m_port, request.pathUrl.c_str());
      return MHD_NO;

    case HTTPRedirect:
      ret = CreateRedirect(request.connection, handler->GetRedirectUrl(), response);
      break;

    case HTTPFileDownload:
      ret = CreateFileDownloadResponse(handler, response);
      break;

    case HTTPMemoryDownloadNoFreeNoCopy:
    case HTTPMemoryDownloadNoFreeCopy:
    case HTTPMemoryDownloadFreeNoCopy:
    case HTTPMemoryDownloadFreeCopy:
      ret = CreateMemoryDownloadResponse(handler, response);
      break;

    case HTTPError:
      ret = CreateErrorResponse(request.connection, responseDetails.status, request.method, response);
      break;

    default:
      CLog::Log(LOGERROR, "CWebServer[%hu]: internal error while HTTP request handler processed %s", m_port, request.pathUrl.c_str());
      return SendErrorResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, request.method);
  }

  if (ret == MHD_NO)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: failed to create HTTP response for %s", m_port, request.pathUrl.c_str());
    return SendErrorResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, request.method);
  }

  return FinalizeRequest(handler, responseDetails.status, response);
}

int CWebServer::FinalizeRequest(const std::shared_ptr<IHTTPRequestHandler>& handler, int responseStatus, struct MHD_Response *response)
{
  if (handler == nullptr || response == nullptr)
    return MHD_NO;

  const HTTPRequest &request = handler->GetRequest();
  const HTTPResponseDetails &responseDetails = handler->GetResponseDetails();

  // if the request handler has set a content type and it hasn't been set as a header, add it 
  if (!responseDetails.contentType.empty())
    handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_TYPE, responseDetails.contentType);

  // if the request handler has set a last modified date and it hasn't been set as a header, add it
  CDateTime lastModified;
  if (handler->GetLastModifiedDate(lastModified) && lastModified.IsValid())
    handler->AddResponseHeader(MHD_HTTP_HEADER_LAST_MODIFIED, lastModified.GetAsRFC1123DateTime());

  // check if the request handler has set Cache-Control and add it if not
  if (!handler->HasResponseHeader(MHD_HTTP_HEADER_CACHE_CONTROL))
  {
    int maxAge = handler->GetMaximumAgeForCaching();
    if (handler->CanBeCached() && maxAge == 0 && !responseDetails.contentType.empty())
    {
      // don't cache HTML, CSS and JavaScript files
      if (!StringUtils::EqualsNoCase(responseDetails.contentType, "text/html") &&
          !StringUtils::EqualsNoCase(responseDetails.contentType, "text/css") &&
          !StringUtils::EqualsNoCase(responseDetails.contentType, "application/javascript"))
        maxAge = CDateTimeSpan(365, 0, 0, 0).GetSecondsTotal();
    }

    // if the response can't be cached or the maximum age is 0 force the client not to cache
    if (!handler->CanBeCached() || maxAge == 0)
      handler->AddResponseHeader(MHD_HTTP_HEADER_CACHE_CONTROL, "private, max-age=0, " HEADER_VALUE_NO_CACHE);
    else
    {
      // create the value of the Cache-Control header
      std::string cacheControl = StringUtils::Format("public, max-age=%d", maxAge);

      // check if the response contains a Set-Cookie header because they must not be cached
      if (handler->HasResponseHeader(MHD_HTTP_HEADER_SET_COOKIE))
        cacheControl += ", no-cache=\"set-cookie\"";

      // set the Cache-Control header
      handler->AddResponseHeader(MHD_HTTP_HEADER_CACHE_CONTROL, cacheControl);

      // set the Expires header
      CDateTime expiryTime = CDateTime::GetCurrentDateTime() + CDateTimeSpan(0, 0, 0, maxAge);
      handler->AddResponseHeader(MHD_HTTP_HEADER_EXPIRES, expiryTime.GetAsRFC1123DateTime());
    }
  }

  // if the request handler can handle ranges and it hasn't been set as a header, add it
  if (handler->CanHandleRanges())
    handler->AddResponseHeader(MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
  else
    handler->AddResponseHeader(MHD_HTTP_HEADER_ACCEPT_RANGES, "none");

  // add MHD_HTTP_HEADER_CONTENT_LENGTH
  if (responseDetails.totalLength > 0)
    handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_LENGTH, StringUtils::Format("%" PRIu64, responseDetails.totalLength));

  // add all headers set by the request handler
  for (std::multimap<std::string, std::string>::const_iterator it = responseDetails.headers.begin(); it != responseDetails.headers.end(); ++it)
    AddHeader(response, it->first, it->second);

  return SendResponse(request, responseStatus, response);
}

std::shared_ptr<IHTTPRequestHandler> CWebServer::FindRequestHandler(const HTTPRequest& request) const
{
  // look for a IHTTPRequestHandler which can take care of the current request
  auto requestHandlerIt = std::find_if(m_requestHandlers.cbegin(), m_requestHandlers.cend(),
    [&request](const IHTTPRequestHandler* requestHandler)
    {
      return requestHandler->CanHandleRequest(request);
    });

  // we found a matching IHTTPRequestHandler so let's get a new instance for this request
  if (requestHandlerIt != m_requestHandlers.cend())
    return std::shared_ptr<IHTTPRequestHandler>((*requestHandlerIt)->Create(request));

  return nullptr;
}

bool CWebServer::IsRequestCacheable(const HTTPRequest& request) const
{
  // handle Cache-Control
  std::string cacheControl = HTTPRequestHandlerUtils::GetRequestHeaderValue(request.connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CACHE_CONTROL);
  if (!cacheControl.empty())
  {
    std::vector<std::string> cacheControls = StringUtils::Split(cacheControl, ",");
    for (auto control : cacheControls)
    {
      control = StringUtils::Trim(control);

      // handle no-cache
      if (control.compare(HEADER_VALUE_NO_CACHE) == 0)
        return false;
    }
  }

  // handle Pragma
  std::string pragma = HTTPRequestHandlerUtils::GetRequestHeaderValue(request.connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_PRAGMA);
  if (pragma.compare(HEADER_VALUE_NO_CACHE) == 0)
    return false;

  return true;
}

bool CWebServer::IsRequestRanged(const HTTPRequest& request, const CDateTime &lastModified) const
{
  // parse the Range header and store it in the request object
  CHttpRanges ranges;
  bool ranged = ranges.Parse(HTTPRequestHandlerUtils::GetRequestHeaderValue(request.connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_RANGE));

  // handle If-Range header but only if the Range header is present
  if (ranged && lastModified.IsValid())
  {
    std::string ifRange = HTTPRequestHandlerUtils::GetRequestHeaderValue(request.connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_IF_RANGE);
    if (!ifRange.empty() && lastModified.IsValid())
    {
      CDateTime ifRangeDate;
      ifRangeDate.SetFromRFC1123DateTime(ifRange);

      // check if the last modification is newer than the If-Range date
      // if so we have to server the whole file instead
      if (lastModified.GetAsUTCDateTime() > ifRangeDate)
        ranges.Clear();
    }
  }

  return !ranges.IsEmpty();
}

void CWebServer::SetupPostDataProcessing(const HTTPRequest& request, ConnectionHandler *connectionHandler, std::shared_ptr<IHTTPRequestHandler> handler, void **con_cls) const
{
  connectionHandler->requestHandler = handler;

  // we might need to handle the POST data ourselves which is done in the next call to AnswerToConnection
  *con_cls = connectionHandler;

  // get the content-type of the POST data
  const auto contentType = HTTPRequestHandlerUtils::GetRequestHeaderValue(request.connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
  if (contentType.empty())
    return;

  // if the content-type is neither application/x-ww-form-urlencoded nor multipart/form-data we need to handle it ourselves
  if (!StringUtils::EqualsNoCase(contentType, MHD_HTTP_POST_ENCODING_FORM_URLENCODED) &&
      !StringUtils::EqualsNoCase(contentType, MHD_HTTP_POST_ENCODING_MULTIPART_FORMDATA))
    return;

  // otherwise we can use MHD's POST processor
  connectionHandler->postprocessor = MHD_create_post_processor(request.connection, MAX_POST_BUFFER_SIZE, &CWebServer::HandlePostField, static_cast<void*>(connectionHandler));

  // MHD doesn't seem to be able to handle this post request
  if (connectionHandler->postprocessor == nullptr)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: unable to create HTTP POST processor for %s", m_port, request.pathUrl.c_str());
    connectionHandler->errorStatus = MHD_HTTP_INTERNAL_SERVER_ERROR;
  }
}

bool CWebServer::ProcessPostData(const HTTPRequest& request, ConnectionHandler *connectionHandler, const char *upload_data, size_t *upload_data_size, void **con_cls) const
{
  if (connectionHandler->requestHandler == nullptr)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: cannot handle partial HTTP POST for %s request because there is no valid request handler available", m_port, request.pathUrl.c_str());
    connectionHandler->errorStatus = MHD_HTTP_INTERNAL_SERVER_ERROR;
  }

  // we only need to handle POST data if there actually is data left to handle
  if (*upload_data_size == 0)
    return false;

  // we may need to handle more POST data which is done in the next call to AnswerToConnection
  *con_cls = connectionHandler;

  // if nothing has gone wrong so far, process the given POST data
  if (connectionHandler->errorStatus == MHD_HTTP_OK)
  {
    bool postDataHandled = false;
    // either use MHD's POST processor
    if (connectionHandler->postprocessor != nullptr)
      postDataHandled = MHD_post_process(connectionHandler->postprocessor, upload_data, *upload_data_size) == MHD_YES;
    // or simply copy the data to the handler
    else if (connectionHandler->requestHandler != nullptr)
      postDataHandled = connectionHandler->requestHandler->AddPostData(upload_data, *upload_data_size);

    // abort if the received POST data couldn't be handled
    if (!postDataHandled)
    {
      CLog::Log(LOGERROR, "CWebServer[%hu]: failed to handle HTTP POST data for %s", m_port, request.pathUrl.c_str());
#if (MHD_VERSION >= 0x00095213)
      connectionHandler->errorStatus = MHD_HTTP_PAYLOAD_TOO_LARGE;
#else
      connectionHandler->errorStatus = MHD_HTTP_REQUEST_ENTITY_TOO_LARGE;
#endif
    }
  }

  // signal that we have handled the data
  *upload_data_size = 0;

  return true;
}

void CWebServer::FinalizePostDataProcessing(ConnectionHandler *connectionHandler) const
{
  if (connectionHandler->postprocessor == nullptr)
    return;

  MHD_destroy_post_processor(connectionHandler->postprocessor);
}

int CWebServer::CreateMemoryDownloadResponse(const std::shared_ptr<IHTTPRequestHandler>& handler, struct MHD_Response *&response) const
{
  if (handler == nullptr)
    return MHD_NO;

  const HTTPRequest &request = handler->GetRequest();
  const HTTPResponseDetails &responseDetails = handler->GetResponseDetails();
  HttpResponseRanges responseRanges = handler->GetResponseData();

  // check if the response is completely empty
  if (responseRanges.empty())
    return CreateMemoryDownloadResponse(request.connection, nullptr, 0, false, false, response);

  // check if the response contains more ranges than the request asked for
  if ((request.ranges.IsEmpty() && responseRanges.size() > 1) ||
     (!request.ranges.IsEmpty() && responseRanges.size() > request.ranges.Size()))
  {
    CLog::Log(LOGWARNING, "CWebServer[%hu]: response contains more ranges (%d) than the request asked for (%d)", m_port, (int)responseRanges.size(), (int)request.ranges.Size());
    return SendErrorResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, request.method);
  }

  // if the request asked for no or only one range we can simply use MHDs memory download handler
  // we MUST NOT send a multipart response
  if (request.ranges.Size() <= 1)
  {
    CHttpResponseRange responseRange = responseRanges.front();
    // check if the range is valid
    if (!responseRange.IsValid())
    {
      CLog::Log(LOGWARNING, "CWebServer[%hu]: invalid response data with range start at %" PRId64 " and end at %" PRId64, m_port, responseRange.GetFirstPosition(), responseRange.GetLastPosition());
      return SendErrorResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, request.method);
    }

    const void* responseData = responseRange.GetData();
    size_t responseDataLength = static_cast<size_t>(responseRange.GetLength());

    switch (responseDetails.type)
    {
    case HTTPMemoryDownloadNoFreeNoCopy:
      return CreateMemoryDownloadResponse(request.connection, responseData, responseDataLength, false, false, response);

    case HTTPMemoryDownloadNoFreeCopy:
      return CreateMemoryDownloadResponse(request.connection, responseData, responseDataLength, false, true, response);

    case HTTPMemoryDownloadFreeNoCopy:
      return CreateMemoryDownloadResponse(request.connection, responseData, responseDataLength, true, false, response);

    case HTTPMemoryDownloadFreeCopy:
      return CreateMemoryDownloadResponse(request.connection, responseData, responseDataLength, true, true, response);

    default:
      return SendErrorResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, request.method);
    }
  }

  return CreateRangedMemoryDownloadResponse(handler, response);
}

int CWebServer::CreateRangedMemoryDownloadResponse(const std::shared_ptr<IHTTPRequestHandler>& handler, struct MHD_Response *&response) const
{
  if (handler == nullptr)
    return MHD_NO;

  const HTTPRequest &request = handler->GetRequest();
  const HTTPResponseDetails &responseDetails = handler->GetResponseDetails();
  HttpResponseRanges responseRanges = handler->GetResponseData();

  // if there's no or only one range this is not the right place
  if (responseRanges.size() <= 1)
    return CreateMemoryDownloadResponse(handler, response);

  // extract all the valid ranges and calculate their total length
  uint64_t firstRangePosition = 0;
  HttpResponseRanges ranges;
  for (HttpResponseRanges::const_iterator range = responseRanges.begin(); range != responseRanges.end(); ++range)
  {
    // ignore invalid ranges
    if (!range->IsValid())
      continue;

    // determine the first range position
    if (ranges.empty())
      firstRangePosition = range->GetFirstPosition();

    ranges.push_back(*range);
  }

  if (ranges.empty())
    return CreateMemoryDownloadResponse(request.connection, nullptr, 0, false, false, response);

  // determine the last range position
  uint64_t lastRangePosition = ranges.back().GetLastPosition();

  // adjust the HTTP status of the response
  handler->SetResponseStatus(MHD_HTTP_PARTIAL_CONTENT);
  // add Content-Range header
  handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_RANGE,
    HttpRangeUtils::GenerateContentRangeHeaderValue(firstRangePosition, lastRangePosition, responseDetails.totalLength));

  // generate a multipart boundary
  std::string multipartBoundary = HttpRangeUtils::GenerateMultipartBoundary();
  // and the content-type
  std::string contentType = HttpRangeUtils::GenerateMultipartBoundaryContentType(multipartBoundary);

  // add Content-Type header
  handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_TYPE, contentType);

  // generate the multipart boundary with the Content-Type header field
  std::string multipartBoundaryWithHeader = HttpRangeUtils::GenerateMultipartBoundaryWithHeader(multipartBoundary, contentType);

  std::string result;
  // add all the ranges to the result
  for (HttpResponseRanges::const_iterator range = ranges.begin(); range != ranges.end(); ++range)
  {
    // add a newline before any new multipart boundary
    if (range != ranges.begin())
      result += HEADER_NEWLINE;

    // generate and append the multipart boundary with the full header (Content-Type and Content-Length)
    result += HttpRangeUtils::GenerateMultipartBoundaryWithHeader(multipartBoundaryWithHeader, &*range);

    // and append the data of the range
    result.append(static_cast<const char*>(range->GetData()), static_cast<size_t>(range->GetLength()));

    // check if we need to free the range data
    if (responseDetails.type == HTTPMemoryDownloadFreeNoCopy || responseDetails.type == HTTPMemoryDownloadFreeCopy)
      free(const_cast<void*>(range->GetData()));
  }

  result += HttpRangeUtils::GenerateMultipartBoundaryEnd(multipartBoundary);

  // add Content-Length header
  handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_LENGTH, StringUtils::Format("%" PRIu64, static_cast<uint64_t>(result.size())));

  // finally create the response
  return CreateMemoryDownloadResponse(request.connection, result.c_str(), result.size(), false, true, response);
}

int CWebServer::CreateRedirect(struct MHD_Connection *connection, const std::string &strURL, struct MHD_Response *&response) const
{
  response = create_response(0, nullptr, MHD_NO, MHD_NO);
  if (response == nullptr)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: failed to create HTTP redirect response to %s", m_port, strURL.c_str());
    return MHD_NO;
  }

  AddHeader(response, MHD_HTTP_HEADER_LOCATION, strURL);
  return MHD_YES;
}

int CWebServer::CreateFileDownloadResponse(const std::shared_ptr<IHTTPRequestHandler>& handler, struct MHD_Response *&response) const
{
  if (handler == nullptr)
    return MHD_NO;

  const HTTPRequest &request = handler->GetRequest();
  const HTTPResponseDetails &responseDetails = handler->GetResponseDetails();
  HttpResponseRanges responseRanges = handler->GetResponseData();

  std::shared_ptr<XFILE::CFile> file = std::make_shared<XFILE::CFile>();
  std::string filePath = handler->GetResponseFile();

  if (!file->Open(filePath, XFILE::READ_NO_CACHE))
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: Failed to open %s", m_port, filePath.c_str());
    return SendErrorResponse(request, MHD_HTTP_NOT_FOUND, request.method);
  }

  bool ranged = false;
  uint64_t fileLength = static_cast<uint64_t>(file->GetLength());

  // get the MIME type for the Content-Type header
  std::string mimeType = responseDetails.contentType;
  if (mimeType.empty())
  {
    std::string ext = URIUtils::GetExtension(filePath);
    StringUtils::ToLower(ext);
    mimeType = CreateMimeTypeFromExtension(ext.c_str());
  }

  if (request.method != HEAD)
  {
    uint64_t totalLength = 0;
    std::unique_ptr<HttpFileDownloadContext> context(new HttpFileDownloadContext());
    context->file = file;
    context->contentType = mimeType;
    context->boundaryWritten = false;
    context->writePosition = 0;

    if (handler->IsRequestRanged())
    {
      if (!request.ranges.IsEmpty())
        context->ranges = request.ranges;
      else
        HTTPRequestHandlerUtils::GetRequestedRanges(request.connection, fileLength, context->ranges);
    }

    uint64_t firstPosition = 0;
    uint64_t lastPosition = 0;
    // if there are no ranges, add the whole range
    if (context->ranges.IsEmpty())
      context->ranges.Add(CHttpRange(0, fileLength - 1));
    else
    {
      handler->SetResponseStatus(MHD_HTTP_PARTIAL_CONTENT);

      // we need to remember that we are ranged because the range length might change and won't be reliable anymore for length comparisons
      ranged = true;

      context->ranges.GetFirstPosition(firstPosition);
      context->ranges.GetLastPosition(lastPosition);
    }

    // remember the total number of ranges
    context->rangeCountTotal = context->ranges.Size();
    // remember the total length
    totalLength = context->ranges.GetLength();

    // adjust the MIME type and range length in case of multiple ranges which requires multipart boundaries
    if (context->rangeCountTotal > 1)
    {
      context->boundary = HttpRangeUtils::GenerateMultipartBoundary();
      mimeType = HttpRangeUtils::GenerateMultipartBoundaryContentType(context->boundary);

      // build part of the boundary with the optional Content-Type header
      // "--<boundary>\r\nContent-Type: <content-type>\r\n
      context->boundaryWithHeader = HttpRangeUtils::GenerateMultipartBoundaryWithHeader(context->boundary, context->contentType);
      context->boundaryEnd = HttpRangeUtils::GenerateMultipartBoundaryEnd(context->boundary);

      // for every range, we need to add a boundary with header
      for (HttpRanges::const_iterator range = context->ranges.Begin(); range != context->ranges.End(); ++range)
      {
        // we need to temporarily add the Content-Range header to the boundary to be able to determine the length
        std::string completeBoundaryWithHeader = HttpRangeUtils::GenerateMultipartBoundaryWithHeader(context->boundaryWithHeader, &*range);
        totalLength += completeBoundaryWithHeader.size();

        // add a newline before any new multipart boundary
        if (range != context->ranges.Begin())
          totalLength += strlen(HEADER_NEWLINE);
      }
      // and at the very end a special end-boundary "\r\n--<boundary>--"
      totalLength += context->boundaryEnd.size();
    }

    // set the initial write position
    context->ranges.GetFirstPosition(context->writePosition);

    // create the response object
    response = MHD_create_response_from_callback(totalLength, 2048,
                                                  &CWebServer::ContentReaderCallback,
                                                  context.get(),
                                                  &CWebServer::ContentReaderFreeCallback);
    if (response == nullptr)
    {
      CLog::Log(LOGERROR, "CWebServer[%hu]: failed to create a HTTP response for %s to be filled from %s", m_port, request.pathUrl.c_str(), filePath.c_str());
      return MHD_NO;
    }

    context.release(); // ownership was passed to mhd

    // add Content-Range header
    if (ranged)
      handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_RANGE, HttpRangeUtils::GenerateContentRangeHeaderValue(firstPosition, lastPosition, fileLength));
  }
  else
  {
    response = create_response(0, nullptr, MHD_NO, MHD_NO);
    if (response == nullptr)
    {
      CLog::Log(LOGERROR, "CWebServer[%hu]: failed to create a HTTP HEAD response for %s", m_port, request.pathUrl.c_str());
      return MHD_NO;
    }

    handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_LENGTH, StringUtils::Format("%" PRId64, fileLength));
  }

  // set the Content-Type header
  if (!mimeType.empty())
    handler->AddResponseHeader(MHD_HTTP_HEADER_CONTENT_TYPE, mimeType);

  return MHD_YES;
}

int CWebServer::CreateErrorResponse(struct MHD_Connection *connection, int responseType, HTTPMethod method, struct MHD_Response *&response) const
{
  size_t payloadSize = 0;
  void *payload = nullptr;

  if (method != HEAD)
  {
    switch (responseType)
    {
      case MHD_HTTP_NOT_FOUND:
        payloadSize = strlen(PAGE_FILE_NOT_FOUND);
        payload = (void *)PAGE_FILE_NOT_FOUND;
        break;

      case MHD_HTTP_NOT_IMPLEMENTED:
        payloadSize = strlen(NOT_SUPPORTED);
        payload = (void *)NOT_SUPPORTED;
        break;
    }
  }

  response = create_response(payloadSize, payload, MHD_NO, MHD_NO);
  if (response == nullptr)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: failed to create a HTTP %d error response", m_port, responseType);
    return MHD_NO;
  }

  return MHD_YES;
}

int CWebServer::CreateMemoryDownloadResponse(struct MHD_Connection *connection, const void *data, size_t size, bool free, bool copy, struct MHD_Response *&response) const
{
  response = create_response(size, const_cast<void*>(data), free ? MHD_YES : MHD_NO, copy ? MHD_YES : MHD_NO);
  if (response == nullptr)
  {
    CLog::Log(LOGERROR, "CWebServer[%hu]: failed to create a HTTP download response", m_port);
    return MHD_NO;
  }

  return MHD_YES;
}

int CWebServer::SendResponse(const HTTPRequest& request, int responseStatus, MHD_Response *response) const
{
  LogResponse(request, responseStatus);

  int ret = MHD_queue_response(request.connection, responseStatus, response);
  MHD_destroy_response(response);

  return ret;
}

int CWebServer::SendErrorResponse(const HTTPRequest& request, int errorType, HTTPMethod method) const
{
  struct MHD_Response *response = nullptr;
  int ret = CreateErrorResponse(request.connection, errorType, method, response);
  if (ret == MHD_NO)
    return MHD_NO;

  return SendResponse(request, errorType, response);
}

void* CWebServer::UriRequestLogger(void *cls, const char *uri)
{
  CWebServer *webServer = reinterpret_cast<CWebServer*>(cls);

  // log the full URI
  if (webServer == nullptr)
    CLog::Log(LOGDEBUG, "CWebServer[unknown]: request received for %s", uri);
  else
    webServer->LogRequest(uri);

  // create and return a new connection handler
  return new ConnectionHandler(uri);
}

void CWebServer::LogRequest(const char* uri) const
{
  if (uri == nullptr)
    return;

  CLog::Log(LOGDEBUG, "CWebServer[%hu]: request received for %s", m_port, uri);
}

#if (MHD_VERSION >= 0x00090200)
ssize_t CWebServer::ContentReaderCallback(void *cls, uint64_t pos, char *buf, size_t max)
#elif (MHD_VERSION >= 0x00040001)
int CWebServer::ContentReaderCallback(void *cls, uint64_t pos, char *buf, int max)
#else   //libmicrohttpd < 0.4.0
int CWebServer::ContentReaderCallback(void *cls, size_t pos, char *buf, int max)
#endif
{
  HttpFileDownloadContext *context = (HttpFileDownloadContext *)cls;
  if (context == nullptr || context->file == nullptr)
    return -1;

#if (MHD_VERSION >= 0x00090200)
  CLog::Log(LOGDEBUG, LOGWEBSERVER, "CWebServer [OUT] write maximum %zu bytes from %" PRIu64 " (%" PRIu64 ")", max, context->writePosition, pos);
#else
  CLog::Log(LOGDEBUG, LOGWEBSERVER, "CWebServer [OUT] write maximum %d bytes from %" PRIu64 " (%" PRIu64 ")", max, context->writePosition, pos);
#endif

  // check if we need to add the end-boundary
  if (context->rangeCountTotal > 1 && context->ranges.IsEmpty())
  {
    // put together the end-boundary
    std::string endBoundary = HttpRangeUtils::GenerateMultipartBoundaryEnd(context->boundary);
    if ((unsigned int)max != endBoundary.size())
      return -1;

    // copy the boundary into the buffer
    memcpy(buf, endBoundary.c_str(), endBoundary.size());
    return endBoundary.size();
  }

  CHttpRange range;
  if (context->ranges.IsEmpty() || !context->ranges.GetFirst(range))
    return -1;

  uint64_t start = range.GetFirstPosition();
  uint64_t end = range.GetLastPosition();
  uint64_t maximum = (uint64_t)max;
  int written = 0;

  if (context->rangeCountTotal > 1 && !context->boundaryWritten)
  {
    // add a newline before any new multipart boundary
    if (context->rangeCountTotal > context->ranges.Size())
    {
      size_t newlineLength = strlen(HEADER_NEWLINE);
      memcpy(buf, HEADER_NEWLINE, newlineLength);
      buf += newlineLength;
      written += newlineLength;
      maximum -= newlineLength;
    }

    // put together the boundary for the current range
    std::string boundary = HttpRangeUtils::GenerateMultipartBoundaryWithHeader(context->boundaryWithHeader, &range);

    // copy the boundary into the buffer
    memcpy(buf, boundary.c_str(), boundary.size());
    // advance the buffer position
    buf += boundary.size();
    // update the number of written byte
    written += boundary.size();
    // update the maximum number of bytes
    maximum -= boundary.size();
    context->boundaryWritten = true;
  }

  // check if the current position is within this range
  // if not, set it to the start position
  if (context->writePosition < start || context->writePosition > end)
    context->writePosition = start;
  // adjust the maximum number of read bytes
  maximum = std::min(maximum, end - context->writePosition + 1);

  // seek to the position if necessary
  if (context->file->GetPosition() < 0 || context->writePosition != static_cast<uint64_t>(context->file->GetPosition()))
    context->file->Seek(static_cast<uint64_t>(context->writePosition));

  // read data from the file
  ssize_t res = context->file->Read(buf, static_cast<size_t>(maximum));
  if (res <= 0)
    return -1;

  // add the number of read bytes to the number of written bytes
  written += res;

  CLog::Log(LOGDEBUG, LOGWEBSERVER, "CWebServer [OUT] wrote %d bytes from %" PRIu64 " in range (%" PRIu64 " - %" PRIu64 ")", written, context->writePosition, start, end);

  // update the current write position
  context->writePosition += res;

  // if we have read all the data from the current range
  // remove it from the list
  if (context->writePosition >= end + 1)
  {
    context->ranges.Remove(0);
    context->boundaryWritten = false;
  }

  return written;
}

void CWebServer::ContentReaderFreeCallback(void *cls)
{
  HttpFileDownloadContext *context = (HttpFileDownloadContext *)cls;
  delete context;

  CLog::Log(LOGDEBUG, LOGWEBSERVER, "CWebServer [OUT] done");
}

// local helper
static void panicHandlerForMHD(void* unused, const char* file, unsigned int line, const char *reason)
{
  CLog::Log(LOGSEVERE, "CWebServer: MHD serious error: reason \"%s\" in file \"%s\" at line %ui", reason ? reason : "",
            file ? file : "", line);
  throw std::runtime_error("MHD serious error"); // FIXME: better solution?
}

// local helper
static void logFromMHD(void* unused, const char* fmt, va_list ap)
{
  if (fmt == nullptr || fmt[0] == 0)
    CLog::Log(LOGERROR, "CWebServer: MHD reported error with empty string");
  else
  {
    std::string errDsc = StringUtils::FormatV(fmt, ap);
    if (errDsc.empty())
      CLog::Log(LOGERROR, "CWebServer: MHD reported error with unprintable string \"%s\"", fmt);
    else
    {
      if (errDsc.at(errDsc.length() - 1) == '\n')
        errDsc.erase(errDsc.length() - 1);
      
      // Most common error is "aborted connection", so log it at LOGDEBUG level
      CLog::Log(LOGDEBUG, "CWebServer [MHD]: %s", errDsc.c_str());
    }
  }
}

bool CWebServer::LoadCert(std::string &skey, std::string &scert)
{
  XFILE::CFile file;
  XFILE::auto_buffer buf;
  const char* keyFile = "special://userdata/server.key";
  const char* certFile = "special://userdata/server.pem";

  if (!file.Exists(keyFile) || !file.Exists(certFile))
    return false;

  if (file.LoadFile(keyFile, buf) > 0)
  {
    skey.resize(buf.length());
    skey.assign(buf.get());
    file.Close();
  }
  else
    CLog::Log(LOGDEBUG, "WebServer %s: Error loading: %s", __FUNCTION__, keyFile);

  if (file.LoadFile(certFile, buf) > 0)
  {
    scert.resize(buf.length());
    scert.assign(buf.get());
    file.Close();
  }
  else
    CLog::Log(LOGDEBUG, "WebServer %s: Error loading: %s", __FUNCTION__, certFile);

  if (!skey.empty() && !scert.empty())
  {
    CLog::Log(LOGERROR, "WebServer %s: found server key: %s, certificate: %s, HTTPS support enabled", __FUNCTION__, keyFile, certFile);
    return true;
  }
  return false;
}

struct MHD_Daemon* CWebServer::StartMHD(unsigned int flags, int port)
{
  unsigned int timeout = 60 * 60 * 24;
  const char* ciphers = "NORMAL:-VERS-TLS1.0";

#if MHD_VERSION >= 0x00040500
  MHD_set_panic_func(&panicHandlerForMHD, nullptr);
#endif

  if (CServiceBroker::GetSettings().GetBool(CSettings::SETTING_SERVICES_WEBSERVERSSL) &&
      MHD_is_feature_supported(MHD_FEATURE_SSL) == MHD_YES &&
      LoadCert(m_key, m_cert))
    // SSL enabled
    return MHD_start_daemon(flags |
#if (MHD_VERSION >= 0x00040002) && (MHD_VERSION < 0x00090B01)
                          // use main thread for each connection, can only handle one request at a
                          // time [unless you set the thread pool size]
                          MHD_USE_SELECT_INTERNALLY
#else
                          // one thread per connection
                          // WARNING: set MHD_OPTION_CONNECTION_TIMEOUT to something higher than 1
                          // otherwise on libmicrohttpd 0.4.4-1 it spins a busy loop
                          MHD_USE_THREAD_PER_CONNECTION
#endif
#if (MHD_VERSION >= 0x00095207)
                          | MHD_USE_INTERNAL_POLLING_THREAD /* MHD_USE_THREAD_PER_CONNECTION must be used only with MHD_USE_INTERNAL_POLLING_THREAD since 0.9.54 */
#endif
#if (MHD_VERSION >= 0x00040001)
                          | MHD_USE_DEBUG /* Print MHD error messages to log */
#endif
                          | MHD_USE_SSL
                          ,
                          port,
                          0,
                          0,
                          &CWebServer::AnswerToConnection,
                          this,

#if (MHD_VERSION >= 0x00040002) && (MHD_VERSION < 0x00090B01)
                          MHD_OPTION_THREAD_POOL_SIZE, 4,
#endif
                          MHD_OPTION_CONNECTION_LIMIT, 512,
                          MHD_OPTION_CONNECTION_TIMEOUT, timeout,
                          MHD_OPTION_URI_LOG_CALLBACK, &CWebServer::UriRequestLogger, this,
#if (MHD_VERSION >= 0x00040001)
                          MHD_OPTION_EXTERNAL_LOGGER, &logFromMHD, 0,
#endif // MHD_VERSION >= 0x00040001
                          MHD_OPTION_THREAD_STACK_SIZE, m_thread_stacksize,
                          MHD_OPTION_HTTPS_MEM_KEY, m_key.c_str(),
                          MHD_OPTION_HTTPS_MEM_CERT, m_cert.c_str(),
                          MHD_OPTION_HTTPS_PRIORITIES, ciphers,
                          MHD_OPTION_END);

  // No SSL
  return MHD_start_daemon(flags |
#if (MHD_VERSION >= 0x00040002) && (MHD_VERSION < 0x00090B01)
                          // use main thread for each connection, can only handle one request at a
                          // time [unless you set the thread pool size]
                          MHD_USE_SELECT_INTERNALLY
#else
                          // one thread per connection
                          // WARNING: set MHD_OPTION_CONNECTION_TIMEOUT to something higher than 1
                          // otherwise on libmicrohttpd 0.4.4-1 it spins a busy loop
                          MHD_USE_THREAD_PER_CONNECTION
#endif
#if (MHD_VERSION >= 0x00095207)
                          | MHD_USE_INTERNAL_POLLING_THREAD /* MHD_USE_THREAD_PER_CONNECTION must be used only with MHD_USE_INTERNAL_POLLING_THREAD since 0.9.54 */
#endif
#if (MHD_VERSION >= 0x00040001)
                          | MHD_USE_DEBUG /* Print MHD error messages to log */
#endif
                          ,
                          port,
                          0,
                          0,
                          &CWebServer::AnswerToConnection,
                          this,

#if (MHD_VERSION >= 0x00040002) && (MHD_VERSION < 0x00090B01)
                          MHD_OPTION_THREAD_POOL_SIZE, 4,
#endif
                          MHD_OPTION_CONNECTION_LIMIT, 512,
                          MHD_OPTION_CONNECTION_TIMEOUT, timeout,
                          MHD_OPTION_URI_LOG_CALLBACK, &CWebServer::UriRequestLogger, this,
#if (MHD_VERSION >= 0x00040001)
                          MHD_OPTION_EXTERNAL_LOGGER, &logFromMHD, 0,
#endif // MHD_VERSION >= 0x00040001
                          MHD_OPTION_THREAD_STACK_SIZE, m_thread_stacksize,
                          MHD_OPTION_END);
}

bool CWebServer::Start(uint16_t port, const std::string &username, const std::string &password)
{
  SetCredentials(username, password);
  if (!m_running)
  {
    int v6testSock;
    if ((v6testSock = socket(AF_INET6, SOCK_STREAM, 0)) >= 0)
    {
      closesocket(v6testSock);
      m_daemon_ip6 = StartMHD(MHD_USE_IPv6, port);
    }
    m_daemon_ip4 = StartMHD(0, port);
    
    m_running = (m_daemon_ip6 != nullptr) || (m_daemon_ip4 != nullptr);
    if (m_running)
    {
      m_port = port;
      CLog::Log(LOGNOTICE, "CWebServer[%hu]: Started", m_port);
    }
    else
      CLog::Log(LOGERROR, "CWebServer[%hu]: Failed to start", port);
  }

  return m_running;
}

bool CWebServer::Stop()
{
  if (!m_running)
    return true;

  if (m_daemon_ip6 != nullptr)
    MHD_stop_daemon(m_daemon_ip6);

  if (m_daemon_ip4 != nullptr)
    MHD_stop_daemon(m_daemon_ip4);
    
  m_running = false;
  CLog::Log(LOGNOTICE, "CWebServer[%hu]: Stopped", m_port);
  m_port = 0;

  return true;
}

bool CWebServer::IsStarted()
{
  return m_running;
}

bool CWebServer::WebServerSupportsSSL()
{
  return MHD_is_feature_supported(MHD_FEATURE_SSL) == MHD_YES;
}

void CWebServer::SetCredentials(const std::string &username, const std::string &password)
{
  CSingleLock lock(m_critSection);

  m_authenticationUsername = username;
  m_authenticationPassword = password;
  m_authenticationRequired = !m_authenticationPassword.empty();
}

void CWebServer::RegisterRequestHandler(IHTTPRequestHandler *handler)
{
  if (handler == nullptr)
    return;

  const auto& it = std::find(m_requestHandlers.cbegin(), m_requestHandlers.cend(), handler);
  if (it != m_requestHandlers.cend())
    return;

  m_requestHandlers.push_back(handler);
  std::sort(m_requestHandlers.begin(), m_requestHandlers.end(),
    [](IHTTPRequestHandler* lhs, IHTTPRequestHandler* rhs) { return rhs->GetPriority() < lhs->GetPriority(); });
}

void CWebServer::UnregisterRequestHandler(IHTTPRequestHandler *handler)
{
  if (handler == nullptr)
    return;

  m_requestHandlers.erase(std::remove(m_requestHandlers.begin(), m_requestHandlers.end(), handler), m_requestHandlers.end());
}

void CWebServer::LogRequest(const HTTPRequest& request) const
{
  if (!g_advancedSettings.CanLogComponent(LOGWEBSERVER))
    return;

  std::multimap<std::string, std::string> headerValues;
  HTTPRequestHandlerUtils::GetRequestHeaderValues(request.connection, MHD_HEADER_KIND, headerValues);
  std::multimap<std::string, std::string> getValues;
  HTTPRequestHandlerUtils::GetRequestHeaderValues(request.connection, MHD_GET_ARGUMENT_KIND, getValues);

  CLog::Log(LOGDEBUG, "CWebServer[%hu]  [IN] %s %s %s", m_port, request.version.c_str(), GetHTTPMethod(request.method).c_str(), request.pathUrlFull.c_str());

  if (!getValues.empty())
  {
    std::vector<std::string> values;
    for (const auto get : getValues)
      values.push_back(get.first + " = " + get.second);

    CLog::Log(LOGDEBUG, "CWebServer[%hu]  [IN] Query arguments: %s", m_port, StringUtils::Join(values, "; ").c_str());
  }

  for (const auto header : headerValues)
    CLog::Log(LOGDEBUG, "CWebServer[%hu]  [IN] %s: %s", m_port, header.first.c_str(), header.second.c_str());
}

void CWebServer::LogResponse(const HTTPRequest& request, int responseStatus) const
{
  if (!g_advancedSettings.CanLogComponent(LOGWEBSERVER))
    return;

  std::multimap<std::string, std::string> headerValues;
  HTTPRequestHandlerUtils::GetRequestHeaderValues(request.connection, MHD_HEADER_KIND, headerValues);

  CLog::Log(LOGDEBUG, "CWebServer[%hu] [OUT] %s %d %s", m_port, request.version.c_str(), responseStatus, request.pathUrlFull.c_str());

  for (const auto header : headerValues)
    CLog::Log(LOGDEBUG, "CWebServer[%hu] [OUT] %s: %s", m_port, header.first.c_str(), header.second.c_str());
}

std::string CWebServer::CreateMimeTypeFromExtension(const char *ext)
{
  if (strcmp(ext, ".kar") == 0)
    return "audio/midi";
  if (strcmp(ext, ".tbn") == 0)
    return "image/jpeg";

  return CMime::GetMimeType(ext);
}

int CWebServer::AddHeader(struct MHD_Response *response, const std::string &name, const std::string &value) const
{
  if (response == nullptr || name.empty())
    return 0;

  CLog::Log(LOGDEBUG, LOGWEBSERVER, "CWebServer[%hu] [OUT] %s: %s", m_port, name.c_str(), value.c_str());

  return MHD_add_response_header(response, name.c_str(), value.c_str());
}
#endif
