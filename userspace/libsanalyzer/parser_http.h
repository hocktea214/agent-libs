#ifdef HAS_ANALYZER

//
// A very rudimentary HTTP parser
// XXX replace this with a real http parser.
//
#define HTTP_GET_STR "GET "
#define HTTP_OPTIONS_STR "OPTI"
#define HTTP_HEAD_STR "HEAD"
#define HTTP_POST_STR "POST"
#define HTTP_PUT_STR "PUT "
#define HTTP_DELETE_STR "DELE"
#define HTTP_TRACE_STR "TRAC"
#define HTTP_CONNECT_STR "CONN"
#define HTTP_RESP_STR "HTTP/"

///////////////////////////////////////////////////////////////////////////////
// HTTP parser
///////////////////////////////////////////////////////////////////////////////
class sinsp_protocol_parser
{
public:
	virtual ~sinsp_protocol_parser();
	virtual bool parse_buffer(char* buf, uint32_t buflen) = 0;
};

class sinsp_http_parser : sinsp_protocol_parser
{
public:
	bool parse_buffer(char* buf, uint32_t buflen);

private:
	inline bool check_and_extract(char* buf, uint32_t buflen, char* tosearch, uint32_t tosearchlen);
	inline bool parse_request(char* buf, uint32_t buflen);
	inline bool parse_response(char* buf, uint32_t buflen);

	string m_url;
	string m_agent;
	int32_t m_status_code;
};

#endif // HAS_ANALYZER
