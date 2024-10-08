/*
 * Copyright (C) 2008 Emweb bv, Herent, Belgium.
 *
 * See the LICENSE file for terms of use.
 * In addition to these terms, permission is also granted to use and
 * modify these two files (CgiParser.C and CgiParser.h) so long as the
 * copyright above is maintained, modifications are documented, and
 * credit is given for any use of the library.
 *
 * CGI parser modelled after the PERL implementation cgi-lib.pl 2.18 by
 * Steven E. Brenner with the following original copyright:

# Perl Routines to Manipulate CGI input
# cgi-lib@pobox.com
#
# Copyright (c) 1993-1999 Steven E. Brenner
# Unpublished work.
# Permission granted to use and modify this library so long as the
# copyright above is maintained, modifications are documented, and
# credit is given for any use of the library.
#
# Thanks are due to many people for reporting bugs and suggestions

# For more information, see:
#     http://cgi-lib.stanford.edu/cgi-lib/

 */

#include <fstream>
#include <stdlib.h>

#include <regex>

#include "CgiParser.h"
#include "WebRequest.h"
#include "WebUtils.h"
#include "FileUtils.h"

#include "Wt/WException.h"
#include "Wt/WLogger.h"
#include "Wt/Http/Request.h"

using std::memmove;
using std::strcpy;
using std::strtol;

namespace {
  const std::regex boundary_e("\\bboundary=(?:(?:\"([^\"]+)\")|(\\S+))",
                              std::regex::icase);
  const std::regex name_e("\\bname=(?:(?:\"([^\"]+)\")|([^\\s:;]+))",
                          std::regex::icase);
  const std::regex filename_e("\\bfilename=(?:(?:\"([^\"]*)\")|([^\\s:;]+))",
                              std::regex::icase);
  const std::regex content_e("^\\s*Content-type:"
                             "\\s*(?:(?:\"([^\"]+)\")|([^\\s:;]+))",
                             std::regex::icase);
  const std::regex content_disposition_e("^\\s*Content-Disposition:",
                                         std::regex::icase);
  const std::regex content_type_e("^\\s*Content-Type:",
                                  std::regex::icase);

  bool fishValue(const std::string& text,
                 const std::regex& e, std::string& result)
  {
    std::smatch what;

    if (std::regex_search(text, what, e)) {
      result = std::string(what[1]) + std::string(what[2]);
      return true;
    } else
      return false;
  }

  bool regexMatch(const std::string& text, const std::regex& e)
  {
    return std::regex_search(text, e);
  }
}

namespace Wt {

WT_MAYBE_UNUSED LOGGER("CgiParser");

CgiParser::CgiParser(::int64_t maxRequestSize, ::int64_t maxFormData)
  : maxFormData_(maxFormData),
    maxRequestSize_(maxRequestSize)
{ }

void CgiParser::parse(WebRequest& request, ReadOption readOption)
{
  /*
   * TODO: optimize this ...
   */
  request_ = &request;

  ::int64_t len = request.contentLength();
  const char *type = request.contentType();
  const char *meth = request.requestMethod();

  request.postDataExceeded_ = (len > maxRequestSize_ ? len : 0);

  std::string queryString = request.queryString();

  LOG_DEBUG("queryString (len=" << len << "): " << queryString);

  if (!queryString.empty() && request_->parameters_.empty()) {
    Http::Request::parseFormUrlEncoded(queryString, request_->parameters_);
  }

  // XDomainRequest cannot set a contentType header, we therefore pass it
  // as a request parameter
  if (readOption != ReadHeadersOnly &&
      strcmp(meth, "POST") == 0 &&
      ((type && strstr(type, "application/x-www-form-urlencoded") == type) ||
       (queryString.find("&contentType=x-www-form-urlencoded") !=
        std::string::npos))) {
    /*
     * TODO: parse this stream-based to avoid the malloc here. For now
     * we protect the maximum that can be POST'ed as form data.
     */
    if (len > maxFormData_)
      throw WException("Oversized application/x-www-form-urlencoded ("
                       + std::to_string(len) + ")");

    auto buf = std::unique_ptr<char[]>(new char[len + 1]);

    request.in().read(buf.get(), len);

    if (request.in().gcount() != (int)len) {
      throw WException("Unexpected short read.");
    }

    buf[len] = 0;

    // This is a special Wt feature, I do not think it standard.
    // For POST, parameters in url-encoded URL are still parsed.

    std::string formQueryString = buf.get();

    LOG_DEBUG("formQueryString (len=" << len << "): " << formQueryString);
    if (!formQueryString.empty()) {
      Http::Request::parseFormUrlEncoded(formQueryString, request_->parameters_);
    }
    Http::ParameterMap::const_iterator it = request_->parameters_.find("Wt-params");
    if (it != request_->parameters_.end() && it->second.size() == 1) {
      Http::Request::parseFormUrlEncoded(it->second[0], request_->parameters_);
    }
  }

  if (readOption != ReadHeadersOnly &&
      type && strstr(type, "multipart/form-data") == type) {
    if (strcmp(meth, "POST") != 0) {
      throw WException("Invalid method for multipart/form-data: "
                       + std::string(meth));
    }

    if (!request.postDataExceeded_)
      readMultipartData(request, type, len);
    else if (readOption == ReadBodyAnyway) {
      for (;len > 0;) {
        ::int64_t toRead = std::min(::int64_t(BUFSIZE), len);
        request.in().read(buf_, toRead);
        if (request.in().gcount() != (::int64_t)toRead)
          throw WException("CgiParser: short read");
        len -= toRead;
      }
    }
  }
}

void CgiParser::readMultipartData(WebRequest& request,
                                  const std::string type, ::int64_t len)
{
  std::string boundary;

  if (!fishValue(type, boundary_e, boundary))
    throw WException("Could not find a boundary for multipart data.");

  boundary = "--" + boundary;

  buflen_ = 0;
  left_ = len;
  spoolStream_ = 0;
  currentKey_.clear();

  if (!parseBody(request, boundary))
    return;

  for (;;) {
    if (!parseHead(request))
      break;
    if (!parseBody(request,boundary))
      break;
  }
}

/*
 * Read until finding the boundary, saving to resultString or
 * resultFile. The boundary itself is not consumed.
 *
 * tossAtBoundary controls how many characters extra (<0)
 * or few (>0) are saved at the start of the boundary in the result.
 */
void CgiParser::readUntilBoundary(WebRequest& request,
                                  const std::string boundary,
                                  int tossAtBoundary,
                                  std::string *resultString,
                                  std::ostream *resultFile)
{
  int bpos;

  while ((bpos = index(boundary)) == -1) {
    /*
     * If we couldn't find it. We need to wind the buffer, but only save
     * not including the boundary length.
     */
    if (left_ == 0)
      throw WException("CgiParser: reached end of input while seeking end of "
                       "headers or content. Format of CGI input is wrong");

    /* save (up to) BUFSIZE from buffer to file or value string, but
     * mind the boundary length */
    int save = std::min((buflen_ - (int)boundary.length()), (int)BUFSIZE);

    if (save > 0) {
      if (resultString)
        *resultString += std::string(buf_, save);
      if (resultFile)
        resultFile->write(buf_, save);

      /* wind buffer */
      windBuffer(save);
    }

    unsigned amt = static_cast<unsigned>
      (std::min(left_,
                static_cast< ::int64_t >(BUFSIZE + MAXBOUND - buflen_)));

    request.in().read(buf_ + buflen_, amt);
    if (request.in().gcount() != (int)amt)
      throw WException("CgiParser: short read");

    left_ -= amt;
    buflen_ += amt;
  }

  if (resultString)
    *resultString += std::string(buf_, bpos - tossAtBoundary);
  if (resultFile)
    resultFile->write(buf_, bpos - tossAtBoundary);

  /* wind buffer */
  windBuffer(bpos);
}

void CgiParser::windBuffer(int offset)
{
  if (offset < buflen_) {
    memmove(buf_, buf_ + offset, buflen_ - offset);
    buflen_ -= offset;
  } else
    buflen_ = 0;
}

int CgiParser::index(const std::string search)
{
  std::string bufS = std::string(buf_, buflen_);

  std::string::size_type i = bufS.find(search);

  if (i == std::string::npos)
    return -1;
  else
    return i;
}

bool CgiParser::parseHead(WebRequest& request)
{
  std::string head;
  readUntilBoundary(request, "\r\n\r\n", -2, &head, 0);

  std::string name;
  std::string fn;
  std::string ctype;

  for (unsigned current = 0; current < head.length();) {
    /* read line by line */
    std::string::size_type i = head.find("\r\n", current);
    const std::string text = head.substr(current, (i == std::string::npos
                                                   ? std::string::npos
                                                   : i - current));

    if (regexMatch(text, content_disposition_e)) {
      fishValue(text, name_e, name);
      fishValue(text, filename_e, fn);
    }

    if (regexMatch(text, content_type_e)) {
      fishValue(text, content_e, ctype);
    }

    current = i + 2;
  }

  LOG_DEBUG("name: " << name << " ct: " << ctype  << " fn: " << fn);

  currentKey_ = name;

  if (!fn.empty()) {
    if (!request.postDataExceeded_) {
      /*
       * It is not easy to create a std::ostream pointing to a
       * temporary file name.
       */
      std::string spool = FileUtils::createTempFileName();

      spoolStream_ = new std::ofstream(spool.c_str(),
        std::ios::out | std::ios::binary);

      request_->files_.insert
        (std::make_pair(name, Http::UploadedFile(spool, fn, ctype)));

      LOG_DEBUG("spooling file to " << spool.c_str());

    } else {
      spoolStream_ = 0;
      // Clear currentKey so that file we don't do harm by reading this
      // giant blob in memory
      currentKey_ = "";
    }
  }

  windBuffer(4);

  return true;
}

bool CgiParser::parseBody(WebRequest& request, const std::string boundary)
{
  std::string value;

  readUntilBoundary(request, boundary, 2,
                    spoolStream_ ? 0 : (!currentKey_.empty() ? &value : 0),
                    spoolStream_);

  if (spoolStream_) {
    LOG_DEBUG("completed spooling");
    delete spoolStream_;
    spoolStream_ = 0;
  } else {
    if (!currentKey_.empty()) {
      LOG_DEBUG("value: \"" << value << "\"");
      request_->parameters_[currentKey_].push_back(value);
    }
  }

  currentKey_.clear();

  if (std::string(buf_ + boundary.length(), 2) == "--")
    return false;

  windBuffer(boundary.length() + 2);

  return true;
}

} // namespace Wt
