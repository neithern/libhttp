#ifndef _http_trace_h_
#define _http_trace_h_

#if defined DEBUG || defined _DEBUG
#define trace printf
#else
#define trace(...)
#endif

#endif // _http_trace_h_