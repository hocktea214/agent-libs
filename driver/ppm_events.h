#ifndef EVENTS_H_
#define EVENTS_H_

//
// Various crap that a callback might need
//
struct event_filler_arguments
{
	char* buffer;	// the buffer that will be filled with the data
	uint32_t buffer_size;	// the space in the ring buffer available for this event
	uint32_t syscall_id;	// the system call ID
#ifdef PPM_ENABLE_SENTINEL
	uint32_t sentinel;
#endif
	uint32_t nevents;
	uint32_t curarg;
	uint32_t nargs;
	uint32_t arg_data_offset;
	uint32_t arg_data_size;
	enum ppm_event_type event_type;	// the event type
	struct pt_regs *regs; // the registers containing the call arguments
	char* str_storage; // String storage. Size is one page.
#ifndef __x86_64__
	unsigned long socketcall_args[6];
#endif
};

//
// Filler table-related definitions
//
#define PPM_AUTOFILL NULL
#define PPM_MAX_AUTOFILL_ARGS 4

//
// Return codes
//
#define PPM_SUCCESS 0
#define PPM_FAILURE_BUFFER_FULL -1
#define PPM_FAILURE_INVALID_USER_MEMORY -2
#define PPM_FAILURE_BUG -3

typedef int32_t (*filler_callback) (struct event_filler_arguments* args);

struct ppm_autofill_arg
{
#define AF_ID_RETVAL -1
#define AF_ID_USEDEFAULT -2
	int16_t id;
	long default_val;
};

enum autofill_paramtype
{
	APT_REG,
	APT_SOCK,	
};

struct ppm_event_entry
{
	filler_callback filler_callback;
	uint16_t n_autofill_args;
	enum autofill_paramtype paramtype;
	struct ppm_autofill_arg autofill_args[PPM_MAX_AUTOFILL_ARGS];
};

extern const struct ppm_event_entry g_ppm_events[];

//
// Functions
//
int32_t f_sys_autofill(struct event_filler_arguments* args, const struct ppm_event_entry* evinfo);
int32_t val_to_ring(struct event_filler_arguments* args, uint64_t val, uint16_t val_len, bool fromuser);
inline int32_t add_sentinel(struct event_filler_arguments* args);
char* npm_getcwd(char *buf, unsigned long bufsize);
uint16_t pack_addr(struct sockaddr* usrsockaddr, int ulen, char* targetbuf, uint16_t targetbufsize);
uint16_t fd_to_socktuple(int fd, struct sockaddr* usrsockaddr, int ulen, bool use_userdata, bool is_inbound, char* targetbuf, uint16_t targetbufsize);
int addr_to_kernel(void __user *uaddr, int ulen, struct sockaddr *kaddr);

#endif /* EVENTS_H_ */
