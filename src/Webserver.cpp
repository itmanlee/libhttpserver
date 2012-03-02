/*
     This file is part of libhttpserver
     Copyright (C) 2011 Sebastiano Merlino

     This library is free software; you can redistribute it and/or
     modify it under the terms of the GNU Lesser General Public
     License as published by the Free Software Foundation; either
     version 2.1 of the License, or (at your option) any later version.

     This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     Lesser General Public License for more details.

     You should have received a copy of the GNU Lesser General Public
     License along with this library; if not, write to the Free Software
     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/
#include "Webserver.hpp"
#include "HttpUtils.hpp"
#include "iostream"
#include "string_utilities.hpp"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#ifdef WITH_PYTHON
#include <Python.h>
#endif

using namespace std;

namespace httpserver {

using namespace http;

int policyCallback (void *, const struct sockaddr*, socklen_t);
void error_log(void*, const char*, va_list);
void* uri_log(void*, const char*);
void access_log(Webserver*, string);
size_t unescaper_func(void*, struct MHD_Connection*, char*);
size_t internal_unescaper(void*, char*);

static void catcher (int sig)
{
}

static void ignore_sigpipe ()
{
    struct sigaction oldsig;
    struct sigaction sig;

    sig.sa_handler = &catcher;
    sigemptyset (&sig.sa_mask);
#ifdef SA_INTERRUPT
    sig.sa_flags = SA_INTERRUPT;  /* SunOS */
#else
    sig.sa_flags = SA_RESTART;
#endif
    if (0 != sigaction (SIGPIPE, &sig, &oldsig))
        fprintf (stderr, "Failed to install SIGPIPE handler: %s\n", strerror (errno));
}

//LOGGING DELEGATE
LoggingDelegate::LoggingDelegate() {}

LoggingDelegate::~LoggingDelegate() {}

void LoggingDelegate::log_access(const string& s) const {}

void LoggingDelegate::log_error(const string& s) const {}

//REQUEST VALIDATOR
RequestValidator::RequestValidator() {}

RequestValidator::~RequestValidator() {}

bool RequestValidator::validate(const string& address) const { return true; }

//UNESCAPER
Unescaper::Unescaper() {}

Unescaper::~Unescaper() {}

void Unescaper::unescape(char* s) const {}

//WEBSERVER
Webserver::Webserver 
(
	int port, 
    const HttpUtils::StartMethod_T& startMethod,
	int maxThreads, 
	int maxConnections,
	int memoryLimit,
	int connectionTimeout,
	int perIPConnectionLimit,
	const LoggingDelegate* logDelegate,
	const RequestValidator* validator,
	const Unescaper* unescaper,
    const struct sockaddr* bindAddress,
    int bindSocket,
	int maxThreadStackSize,
    bool useSsl,
    bool useIpv6,
    bool debug,
    bool pedantic,
	const string& httpsMemKey,
	const string& httpsMemCert,
	const string& httpsMemTrust,
	const string& httpsPriorities,
	const HttpUtils::CredType_T& credType,
	const string digestAuthRandom,
	int nonceNcSize
) :
	port(port), 
	startMethod(startMethod),
	maxThreads(maxThreads), 
	maxConnections(maxConnections),
	memoryLimit(memoryLimit),
	connectionTimeout(connectionTimeout),
	perIPConnectionLimit(perIPConnectionLimit),
	logDelegate(logDelegate),
	validator(validator),
	unescaper(unescaper),
    bindAddress(bindAddress),
    bindSocket(bindSocket),
    maxThreadStackSize(maxThreadStackSize),
    useSsl(useSsl),
    useIpv6(useIpv6),
    debug(debug),
    pedantic(pedantic),
	httpsMemKey(httpsMemKey),
	httpsMemCert(httpsMemCert),
	httpsMemTrust(httpsMemTrust),
	httpsPriorities(httpsPriorities),
	credType(credType),
	digestAuthRandom(digestAuthRandom),
	nonceNcSize(nonceNcSize),
	running(false)
{
    ignore_sigpipe();
}

Webserver::Webserver(const CreateWebserver& params):
    port(params._port),
	startMethod(params._startMethod),
    maxThreads(params._maxThreads),
    maxConnections(params._maxConnections),
    memoryLimit(params._memoryLimit),
    connectionTimeout(params._connectionTimeout),
    perIPConnectionLimit(params._perIPConnectionLimit),
    logDelegate(params._logDelegate),
    validator(params._validator),
    unescaper(params._unescaper),
    bindAddress(params._bindAddress),
    bindSocket(params._bindSocket),
    maxThreadStackSize(params._maxThreadStackSize),
    useSsl(params._useSsl),
    useIpv6(params._useIpv6),
    debug(params._debug),
    pedantic(params._pedantic),
    httpsMemKey(params._httpsMemKey),
    httpsMemCert(params._httpsMemCert),
    httpsMemTrust(params._httpsMemTrust),
    httpsPriorities(params._httpsPriorities),
    credType(params._credType),
    digestAuthRandom(params._digestAuthRandom),
    nonceNcSize(params._nonceNcSize),
    running(false)
{
    ignore_sigpipe();
}

Webserver::~Webserver()
{
	this->stop();
}

void Webserver::sweetKill()
{
	this->running = false;
}

void Webserver::requestCompleted (void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe) 
{
	ModdedRequest* mr = (struct ModdedRequest*) *con_cls;
	if (NULL == mr) 
	{
		return;
	}
	if (NULL != mr->pp) 
	{
		MHD_destroy_post_processor (mr->pp);
	}
	if(mr->second)
		delete mr->dhr; //TODO: verify. It could be an error
	delete mr->completeUri;
	free(mr);
}

bool Webserver::start(bool blocking)
{
	struct {
		MHD_OptionItem operator ()(enum MHD_OPTION opt, intptr_t val, void *ptr = 0) {
			MHD_OptionItem x = {opt, val, ptr};
			return x;
		}
	} gen;
	vector<struct MHD_OptionItem> iov;

	iov.push_back(gen(MHD_OPTION_NOTIFY_COMPLETED, (intptr_t) &requestCompleted, NULL ));
	iov.push_back(gen(MHD_OPTION_URI_LOG_CALLBACK, (intptr_t) &uri_log, this));
	iov.push_back(gen(MHD_OPTION_EXTERNAL_LOGGER, (intptr_t) &error_log, this));
	iov.push_back(gen(MHD_OPTION_UNESCAPE_CALLBACK, (intptr_t) &unescaper_func, this));
	iov.push_back(gen(MHD_OPTION_CONNECTION_TIMEOUT, connectionTimeout));
    if(bindAddress != 0x0)
        iov.push_back(gen(MHD_OPTION_SOCK_ADDR, (intptr_t) bindAddress));
    if(bindSocket != 0)
        iov.push_back(gen(MHD_OPTION_LISTEN_SOCKET, bindSocket));
	if(maxThreads != 0)
		iov.push_back(gen(MHD_OPTION_THREAD_POOL_SIZE, maxThreads));
	if(maxConnections != 0)
		iov.push_back(gen(MHD_OPTION_CONNECTION_LIMIT, maxConnections));
	if(memoryLimit != 0)
		iov.push_back(gen(MHD_OPTION_CONNECTION_MEMORY_LIMIT, memoryLimit));
	if(perIPConnectionLimit != 0)
		iov.push_back(gen(MHD_OPTION_PER_IP_CONNECTION_LIMIT, perIPConnectionLimit));
	if(maxThreadStackSize != 0)
		iov.push_back(gen(MHD_OPTION_THREAD_STACK_SIZE, maxThreadStackSize));
	if(nonceNcSize != 0)
		iov.push_back(gen(MHD_OPTION_NONCE_NC_SIZE, nonceNcSize));
	if(httpsMemKey != "")
		iov.push_back(gen(MHD_OPTION_HTTPS_MEM_KEY, (intptr_t)httpsMemKey.c_str()));
	if(httpsMemCert != "")
		iov.push_back(gen(MHD_OPTION_HTTPS_MEM_CERT, (intptr_t)httpsMemCert.c_str()));
	if(httpsMemTrust != "")
		iov.push_back(gen(MHD_OPTION_HTTPS_MEM_TRUST, (intptr_t)httpsMemTrust.c_str()));
	if(httpsPriorities != "")
		iov.push_back(gen(MHD_OPTION_HTTPS_PRIORITIES, (intptr_t)httpsPriorities.c_str()));
	if(digestAuthRandom != "")
		iov.push_back(gen(MHD_OPTION_DIGEST_AUTH_RANDOM, digestAuthRandom.size(), (char*)digestAuthRandom.c_str()));
	if(credType != HttpUtils::NONE)
		iov.push_back(gen(MHD_OPTION_HTTPS_CRED_TYPE, credType));

	iov.push_back(gen(MHD_OPTION_END, 0, NULL ));

	struct MHD_OptionItem ops[iov.size()];
	for(unsigned int i = 0; i < iov.size(); i++)
	{
		ops[i] = iov[i];
	}

    int startConf = startMethod;
    if(useSsl)
        startConf |= MHD_USE_SSL;
    if(useIpv6)
        startConf |= MHD_USE_IPv6;
    if(debug)
        startConf |= MHD_USE_DEBUG;
    if(pedantic)
        startConf |= MHD_USE_PEDANTIC_CHECKS;

	this->daemon = MHD_start_daemon
	(
			startConf, this->port, &policyCallback, this,
			&answerToConnection, this, MHD_OPTION_ARRAY, ops, MHD_OPTION_END
	);

	if(NULL == daemon)
	{
		cout << "Unable to connect daemon to port: " << this->port << endl;
		return false;
	}
	this->running = true;
	bool value_onclose = false;
	if(blocking)
	{
		while(blocking && running)
			sleep(1);
		value_onclose = this->stop();
	}
	return value_onclose;
}

bool Webserver::isRunning()
{
	return this->running;
}

bool Webserver::stop()
{
	if(this->running)
	{
		MHD_stop_daemon (this->daemon);
		this->running = false;
	}
	return true;
}

void Webserver::registerResource(const string& resource, HttpResource* http_resource, bool family)
{
	this->registeredResources[HttpEndpoint(resource, family, true)] = http_resource;
}

int Webserver::buildRequestHeader (void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
	HttpRequest* dhr = (HttpRequest*)(cls);
	dhr->setHeader(key, value);
	return MHD_YES;
}

int Webserver::buildRequestCookie (void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
    HttpRequest* dhr = (HttpRequest*)(cls);
    dhr->setCookie(key, value);
    return MHD_YES;
}

int Webserver::buildRequestFooter (void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
	HttpRequest* dhr = (HttpRequest*)(cls);
	dhr->setFooter(key, value);
	return MHD_YES;
}

int Webserver::buildRequestArgs (void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
	ModdedRequest* mr = (ModdedRequest*)(cls);
	int size = internal_unescaper((void*)mr->ws, (char*) value);
	mr->dhr->setArg(key, string(value, size));
	return MHD_YES;
}

int policyCallback (void *cls, const struct sockaddr* addr, socklen_t addrlen)
{
	// TODO: Develop a system that allow to study a configurable policy callback
	return MHD_YES;
}

void* uri_log(void* cls, const char* uri)
{
	struct ModdedRequest* mr = (struct ModdedRequest*) calloc(1,sizeof(struct ModdedRequest));
	mr->completeUri = new string(uri);
	mr->second = false;
	return ((void*)mr);
}

void error_log(void* cls, const char* fmt, va_list ap)
{
	Webserver* dws = (Webserver*) cls;
	if(dws->logDelegate != 0x0)
	{
		dws->logDelegate->log_error(fmt);
	}
	else
	{
		cout << fmt << endl;
	}
}

void access_log(Webserver* dws, string uri)
{
	if(dws->logDelegate != 0x0)
	{
		dws->logDelegate->log_access(uri);
	}
	else
	{
		cout << uri << endl;
	}
}

size_t unescaper_func(void * cls, struct MHD_Connection *c, char *s)
{
	// THIS IS USED TO AVOID AN UNESCAPING OF URL BEFORE THE ANSWER.
	// IT IS DUE TO A BOGUS ON libmicrohttpd (V0.99) THAT PRODUCING A
	// STRING CONTAINING '\0' AFTER AN UNESCAPING, IS UNABLE TO PARSE
	// ARGS WITH get_connection_values FUNC OR lookup FUNC.
	return strlen(s);
}

size_t internal_unescaper(void* cls, char* s)
{
	Webserver* dws = (Webserver*) cls;
	if(dws->unescaper != 0x0)
	{
		dws->unescaper->unescape(s);
		return strlen(s);
	}
	else
	{
		return http_unescape(s);
	}
}

int Webserver::post_iterator (void *cls, enum MHD_ValueKind kind,
	const char *key,
	const char *filename,
	const char *content_type,
	const char *transfer_encoding,
	const char *data, uint64_t off, size_t size
    )
{
	struct ModdedRequest* mr = (struct ModdedRequest*) cls;
	mr->dhr->setArg(key, data, size);
	return MHD_YES;
}

int Webserver::not_found_page (const void *cls,
	struct MHD_Connection *connection)
{
	int ret;
	struct MHD_Response *response;

	/* unsupported HTTP method */
	response = MHD_create_response_from_buffer (strlen (NOT_FOUND_ERROR),
		(void *) NOT_FOUND_ERROR,
		MHD_RESPMEM_PERSISTENT);
	ret = MHD_queue_response (connection, 
		MHD_HTTP_NOT_FOUND, 
		response);
	MHD_add_response_header (response,
		MHD_HTTP_HEADER_CONTENT_ENCODING,
		"application/json");
	MHD_destroy_response (response);
	return ret;
}

int Webserver::method_not_acceptable_page (const void *cls,
	struct MHD_Connection *connection)
{
	int ret;
	struct MHD_Response *response;

	/* unsupported HTTP method */
	response = MHD_create_response_from_buffer (strlen (NOT_METHOD_ERROR),
		(void *) NOT_METHOD_ERROR,
		MHD_RESPMEM_PERSISTENT);
	ret = MHD_queue_response (connection, 
		MHD_HTTP_METHOD_NOT_ACCEPTABLE, 
		response);
	MHD_add_response_header (response,
		MHD_HTTP_HEADER_CONTENT_ENCODING,
		"application/json");
	MHD_destroy_response (response);
	return ret;
}

int Webserver::answerToConnection(void* cls, MHD_Connection* connection,
	const char* url, const char* method,
	const char* version, const char* upload_data,
	size_t* upload_data_size, void** con_cls
    )
{
	struct MHD_Response *response;
	struct ModdedRequest *mr;
	int ret;
	HttpRequest supportReq;
	Webserver* dws = (Webserver*)(cls);
	internal_unescaper(cls, (char*) url);
	string st_url = HttpUtils::standardizeUrl(url);

	mr = (struct ModdedRequest*) *con_cls;
	access_log(dws, *(mr->completeUri) + " METHOD: " + method);
	mr->ws = dws;
	if (0 == strcmp (method, MHD_HTTP_METHOD_POST) || 0 == strcmp(method, MHD_HTTP_METHOD_PUT)) 
	{
		if (mr->second == false) 
		{
			mr->second = true;
			mr->dhr = new HttpRequest();
			const char *encoding = MHD_lookup_connection_value (connection, MHD_HEADER_KIND, HttpUtils::http_header_content_type.c_str());
			//mr->dhr->setHeader(HttpUtils::http_header_content_type, string(encoding));
			if ( 0x0 != encoding && 0 == strcmp(method, MHD_HTTP_METHOD_POST) && ((0 == strncasecmp (MHD_HTTP_POST_ENCODING_FORM_URLENCODED, encoding, strlen (MHD_HTTP_POST_ENCODING_FORM_URLENCODED))))) 
			{
				mr->pp = MHD_create_post_processor (connection, 1024, &post_iterator, mr);
			} 
			else 
			{
				mr->pp = NULL;
			}
			return MHD_YES;
		}
	}
	else 
	{
		supportReq = HttpRequest();
		mr->dhr = &supportReq;
	}

	mr->dhr->setPath(string(st_url));
	mr->dhr->setMethod(string(method));

	MHD_get_connection_values (connection, MHD_HEADER_KIND, &buildRequestHeader, (void*) mr->dhr);
	MHD_get_connection_values (connection, MHD_FOOTER_KIND, &buildRequestFooter, (void*) mr->dhr);
	MHD_get_connection_values (connection, MHD_COOKIE_KIND, &buildRequestCookie, (void*) mr->dhr);

	if (    0 == strcmp(method, MHD_HTTP_METHOD_DELETE) || 
		0 == strcmp(method, MHD_HTTP_METHOD_GET) ||
		0 == strcmp(method, MHD_HTTP_METHOD_HEAD) ||
		0 == strcmp(method, MHD_HTTP_METHOD_CONNECT) ||
		0 == strcmp(method, MHD_HTTP_METHOD_HEAD) ||
		0 == strcmp(method, MHD_HTTP_METHOD_TRACE)
	) 
	{
		MHD_get_connection_values (connection, MHD_GET_ARGUMENT_KIND, &buildRequestArgs, (void*) mr);
	} 
	else if (0 == strcmp (method, MHD_HTTP_METHOD_POST) || 0 == strcmp(method, MHD_HTTP_METHOD_PUT)) 
	{
		string encoding = mr->dhr->getHeader(HttpUtils::http_header_content_type);
		if ( 0 == strcmp(method, MHD_HTTP_METHOD_POST) && ((0 == strncasecmp (MHD_HTTP_POST_ENCODING_FORM_URLENCODED, encoding.c_str(), strlen (MHD_HTTP_POST_ENCODING_FORM_URLENCODED))))) 
		{
			MHD_post_process(mr->pp, upload_data, *upload_data_size);
		}
		if ( 0 != *upload_data_size)
		{
			mr->dhr->growContent(upload_data, *upload_data_size);
			*upload_data_size = 0;
			return MHD_YES;
		} 
	} 
	else 
	{
		return method_not_acceptable_page(cls, connection);
	}

	if (0 == strcmp (method, MHD_HTTP_METHOD_POST) || 0 == strcmp(method, MHD_HTTP_METHOD_PUT)) 
	{
		supportReq = *(mr->dhr);
	} 

	char* pass = NULL;
	char* user = MHD_basic_auth_get_username_password (connection, &pass);
	supportReq.setVersion(version);
	const MHD_ConnectionInfo * conninfo = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
	supportReq.setRequestor(get_ip_str(conninfo->client_addr));
	supportReq.setRequestorPort(get_port(conninfo->client_addr));
	if(pass != NULL)
	{
		supportReq.setPass(string(pass));
		supportReq.setUser(string(user));
	}
	int toRet;
	HttpEndpoint endpoint = HttpEndpoint(st_url);
	HttpResponse dhrs;
	void* page;
	size_t size = 0;
	bool to_free = false;
	if((dws->registeredResources.count(endpoint) > 0)) 
	{
#ifdef WITH_PYTHON
		PyGILState_STATE gstate;
		if(PyEval_ThreadsInitialized())
		{
			gstate = PyGILState_Ensure();
		}
#endif
		dhrs = dws->registeredResources[endpoint]->routeRequest(supportReq);
#ifdef WITH_PYTHON
		if(PyEval_ThreadsInitialized())
		{
			PyGILState_Release(gstate);
		}
#endif
		if(dhrs.content != "")
		{
			vector<char> v_page(dhrs.content.begin(), dhrs.content.end());
			size = v_page.size();
			page = (void*) malloc(size*sizeof(char));
			memcpy( page, &v_page[0], sizeof( char ) * size );
			to_free = true;
		}
		else
		{
			page = (void*) "";
		}
	} 
	else 
	{
		map<HttpEndpoint, HttpResource* >::iterator it;
		int len = -1;
		bool found = false;
		HttpEndpoint matchingEndpoint;
		for(it=dws->registeredResources.begin(); it!=dws->registeredResources.end(); it++) 
		{
			if(len == -1 || ((int)((*it).first.get_url_pieces().size())) > len)
			{
				if((*it).first.match(endpoint))
				{
					found = true;
					len = (*it).first.get_url_pieces().size();
					matchingEndpoint = (*it).first;
				}
			}
		}
		if(!found) 
		{
			toRet = not_found_page(cls, connection);
			if (user != 0x0)
				free (user);
			if (pass != 0x0)
				free (pass);
			return MHD_YES;
		} 
		else 
		{
			vector<string> url_pars = matchingEndpoint.get_url_pars();
			vector<string> url_pieces = endpoint.get_url_pieces();
			vector<int> chunkes = matchingEndpoint.get_chunk_positions();
			for(unsigned int i = 0; i < url_pars.size(); i++) 
			{
				supportReq.setArg(url_pars[i], url_pieces[chunkes[i]]);
			}
#ifdef WITH_PYTHON
			PyGILState_STATE gstate;
			if(PyEval_ThreadsInitialized())
			{
				gstate = PyGILState_Ensure();
			}
#endif
			dhrs = dws->registeredResources[matchingEndpoint]->routeRequest(supportReq);
#ifdef WITH_PYTHON
			if(PyEval_ThreadsInitialized())
			{
				PyGILState_Release(gstate);
			}
#endif
			if(dhrs.content != "")
			{
				vector<char> v_page(dhrs.content.begin(), dhrs.content.end());
				size = v_page.size();
				page = (void*) malloc(size*sizeof(char));
				memcpy( page, &v_page[0], sizeof( char ) * size );
				to_free = true;
			}
			else
			{
				page = (void*)"";
			}
		}
	}
	if(dhrs.responseType == HttpResponse::FILE_CONTENT)
	{
		struct stat st;
		fstat(dhrs.fp, &st);
		size_t filesize = st.st_size;
		response = MHD_create_response_from_fd_at_offset(filesize, dhrs.fp, 0);
	}
	else
		response = MHD_create_response_from_buffer(size, page, MHD_RESPMEM_MUST_COPY);
	vector<pair<string,string> > response_headers = dhrs.getHeaders();
	vector<pair<string,string> > response_footers = dhrs.getFooters();
	vector<pair<string,string> >::iterator it;
	for (it=response_headers.begin() ; it != response_headers.end(); it++)
		MHD_add_response_header(response, (*it).first.c_str(), (*it).second.c_str());
	for (it=response_footers.begin() ; it != response_footers.end(); it++)
		MHD_add_response_footer(response, (*it).first.c_str(), (*it).second.c_str());
	ret = MHD_queue_response(connection, dhrs.getResponseCode(), response);
	toRet = ret;

	if (user != 0x0)
		free (user);
	if (pass != 0x0)
		free (pass);
	MHD_destroy_response (response);
	if(to_free)
		free(page);
	return MHD_YES;
}

};
