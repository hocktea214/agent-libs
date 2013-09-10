#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/param.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#include "scap.h"

#include "scap-int.h"
//
// When a process calls clone(), its child inherits its file descriptors. In order to take that
// into account, we need to copy the fds of the parent into the child list.
// This is what this function does.
//
/*int32_t scap_proc_copy_fd_table(scap_t* handle, scap_threadinfo* dst, scap_threadinfo* src)
{
	struct scap_fdinfo* fdi;
	struct scap_fdinfo* tfdi;
	struct scap_fdinfo* newfdi;
	int32_t uth_status = SCAP_SUCCESS;

	HASH_ITER(hh, src->fdlist, fdi, tfdi)
	{
		//
		// Make sure this fd doesn't already exist in the child table
		//
		HASH_FIND_INT64(dst->fdlist, &fdi->fd, newfdi);
		if(newfdi != NULL)
		{
			//
			// This should really never happen, because we're just copying the parent list which
			// can't have duplicates
			//
			ASSERT(false);
			snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "duplicate fd %"PRIu64"for process%"PRIu64, fdi->fd, dst->tid);
			return SCAP_FAILURE;
		}

		newfdi = (scap_fdinfo*)malloc(sizeof(scap_fdinfo));
		if(newfdi == NULL)
		{
			snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "fd table allocation error (3)");
			return SCAP_FAILURE;
		}

		//
		// Structure-copy the parent's fd into the just allocated one.
		// NOTE: this will definitely be faster if we copy field-by-field, but performance is not critical here
		//       (we just did a malloc...)
		//
		*newfdi = *fdi;

		//
		// Add the new fd to the child's table
		//
		HASH_ADD_INT64(dst->fdlist, fd, newfdi);
		if(uth_status != SCAP_SUCCESS)
		{
			snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "process table allocation error (2)");
			return SCAP_FAILURE;
		}
	}

	return SCAP_SUCCESS;
}
*/

#ifndef _WIN32

int32_t scap_proc_fill_cwd(char* procdirname, struct scap_threadinfo* tinfo)
{
	int target_res;
	char filename[SCAP_MAX_PATH_SIZE];

	snprintf(filename, sizeof(filename), "%scwd", procdirname);

	target_res = readlink(filename, tinfo->cwd, sizeof(tinfo->cwd) - 1);
	if(target_res <= 0)
	{
		return SCAP_FAILURE;
	}
	tinfo->cwd[target_res] = '\0';
	return SCAP_SUCCESS;
}

//
// use prlimit to extract the RLIMIT_NOFILE for the tid. On systems where prlimit
// is not supported, just return -1
//
int32_t scap_proc_fill_flimit(uint64_t tid, struct scap_threadinfo* tinfo)
#ifdef SYS_prlimit64
{
	struct rlimit rl;

	if(syscall(SYS_prlimit64, tid, RLIMIT_NOFILE, NULL, &rl) == 0)
	{
		tinfo->fdlimit = rl.rlim_cur;
		return SCAP_SUCCESS;
	}

	tinfo->fdlimit = -1;
	return SCAP_SUCCESS;
}
#else
{
	tinfo->fdlimit = -1;
	return SCAP_SUCCESS;
}
#endif

//
// Add a process to the list by parsing its entry under /proc
//
int32_t scap_proc_add_from_proc(scap_t* handle, uint32_t tid, int parenttid, int tid_to_scan, char* procdirname, scap_fdinfo* sockets, scap_threadinfo** procinfo, char *error)
{
	char dir_name[256];
	char target_name[256];
	int target_res;
	char filename[252];
	char line[SCAP_MAX_PATH_SIZE];
	struct scap_threadinfo* tinfo;
	int32_t uth_status = SCAP_SUCCESS;
	FILE* f;
	size_t filesize;
	size_t exe_len;

	snprintf(dir_name, sizeof(dir_name), "%s/%u/", procdirname, tid);
	snprintf(filename, sizeof(filename), "%sexe", dir_name);

	//
	// Gather the executable full name
	//
	target_res = readlink(filename, target_name, sizeof(target_name) - 1);			// Getting the target of the exe, i.e. to which binary it points to

	if(target_res <= 0)
	{
		//
		// This is normal and happens with kernel threads, which we aren't interested in
		//
		return SCAP_SUCCESS;
	}

	target_name[target_res] = 0;

	//
	// This is a real user level process. Allocate the procinfo structure.
	//
	tinfo = (scap_threadinfo*)malloc(sizeof(scap_threadinfo));
	if(tinfo == NULL)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "process table allocation error (1)");
		return SCAP_FAILURE;
	}

	tinfo->tid = tid;
	if(parenttid != -1)
	{
		tinfo->pid = parenttid;
	}
	else
	{
		tinfo->pid = tid;
	}
	tinfo->fdlist = NULL;

	//
	// If tid is different from pid, assume this is a thread and that the FDs are shared, and set the
	// corresponding process flags.
	// XXX we should see if the process creation flags are stored somewhere in /proc and handle this
	// properly instead of making assumptions.
	//
	if(tinfo->tid == tinfo->pid)
	{
		tinfo->flags = 0;
	}
	else
	{
		tinfo->flags = PPM_CL_CLONE_THREAD | PPM_CL_CLONE_FILES;
	}

	snprintf(tinfo->exe, SCAP_MAX_PATH_SIZE, "%s", target_name);

	//
	// Gather the command name
	//
	snprintf(filename, sizeof(filename), "%sstatus", dir_name);

	f = fopen(filename, "r");
	if(f == NULL)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "can't open %s", filename);
		free(tinfo);
		return SCAP_FAILURE;
	}
	else
	{
		fgets(line, SCAP_MAX_PATH_SIZE, f);
		line[SCAP_MAX_PATH_SIZE - 1] = 0;
		sscanf(line, "Name:%s", tinfo->comm);
		fclose(f);
	}

	//
	// Gather the command line
	//
	snprintf(filename, sizeof(filename), "%scmdline", dir_name);

	f = fopen(filename, "r");
	if(f == NULL)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "can't open %s", filename);
		free(tinfo);
		return SCAP_FAILURE;
	}
	else
	{
		filesize = fread(line, 1, sizeof(line) - 1, f);
		line[filesize] = 0;
		line[filesize + 1] = 0;

		exe_len = strlen(line);
		if(exe_len < filesize)
		{
			++exe_len;
		}

		tinfo->args_len = filesize - exe_len;
		if(tinfo->args_len > SCAP_MAX_PATH_SIZE)
		{
			tinfo->args_len = SCAP_MAX_PATH_SIZE;
		}

		memcpy(tinfo->args, line + exe_len, tinfo->args_len);
		tinfo->args[SCAP_MAX_PATH_SIZE - 1] = 0;

		fclose(f);
	}

	//
	// set the current working directory of the process
	//
	if(SCAP_FAILURE == scap_proc_fill_cwd(dir_name, tinfo))
	{
		snprintf(error, SCAP_LASTERR_SIZE, "can't fill cwd for %s", dir_name);
		free(tinfo);
		return SCAP_FAILURE;
	}

	//
	// Set the file limit
	//
	if(SCAP_FAILURE == scap_proc_fill_flimit(tinfo->tid, tinfo))
	{
		snprintf(error, SCAP_LASTERR_SIZE, "can't fill flimit for %s", dir_name);
		free(tinfo);
		return SCAP_FAILURE;
	}

	//
	// if tid_to_scan is set we assume is a runtime lookup so no
	// need to use the table
	//
	if(tid_to_scan == -1)
	{
		//
		// Done. Add the entry to the process table
		//
		HASH_ADD_INT64(handle->m_proclist, tid, tinfo);
		if(uth_status != SCAP_SUCCESS)
		{
			snprintf(error, SCAP_LASTERR_SIZE, "process table allocation error (2)");
			return SCAP_FAILURE;
		}
	}
	else
	{
		*procinfo = tinfo;
	}
	
	//
	// Only add fd's for processes, not tasks
	//
	if(-1 == parenttid)
	{
		return scap_fd_scan_fd_dir(handle, dir_name, tinfo, sockets, error);
	}
	return SCAP_SUCCESS;
}

//
// Scan a directory containing multiple processes under /proc
//
int32_t scap_proc_scan_proc_dir(scap_t* handle, char* procdirname, int parenttid, int tid_to_scan, struct scap_threadinfo** procinfo, char *error)
{
	DIR *dir_p;
	struct dirent *dir_entry_p;
	scap_threadinfo* tinfo;
	uint64_t tid;
	int32_t res = SCAP_SUCCESS;
	char childdir[SCAP_MAX_PATH_SIZE];

	scap_fdinfo* sockets = NULL;

	tid = 0;
	dir_p = opendir(procdirname);

	if(dir_p == NULL)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "error opening the %s directory", procdirname);
		return SCAP_NOTFOUND;
	}
	if (-1 == parenttid)
	{
		if(SCAP_FAILURE == scap_fd_read_sockets(handle, &sockets))
		{
			closedir(dir_p);
			return SCAP_FAILURE;
		}
/*		if(SCAP_FAILURE == scap_fd_post_process_unix_sockets(handle,sockets))
		{
			closedir(dir_p);
			scap_fd_free_table(handle, &sockets);
			return SCAP_FAILURE;
		}
*/
	}

	if(tid_to_scan != -1)
	{
		*procinfo = NULL;
	}

	while((dir_entry_p = readdir(dir_p)) != NULL)
	{
		if(strspn(dir_entry_p->d_name, "0123456789") != strlen(dir_entry_p->d_name))
		{
			continue;
		}

		//
		// Gather the process TID, which is the directory name
		//
		tid = atoi(dir_entry_p->d_name);

		if(parenttid != -1 && tid == parenttid)
		{
			continue;
		}

		//
		// if tid_to_scan is set we assume is a runtime lookup so no
		// need to use the table
		//
		if(tid_to_scan == -1)
		{
			HASH_FIND_INT64(handle->m_proclist, &tid, tinfo);
			if(tinfo != NULL)
			{
				ASSERT(false);
				snprintf(error, SCAP_LASTERR_SIZE, "duplicate process %"PRIu64, tid);
				res = SCAP_FAILURE;
				break;
			}
		}

		if(tid_to_scan == -1 || tid_to_scan == tid)
		{
			//
			// We have a process that needs to be explored
			//
			res = scap_proc_add_from_proc(handle, tid, parenttid, tid_to_scan, procdirname, sockets, procinfo, error);
			if(res != SCAP_SUCCESS)
			{
				snprintf(error, SCAP_LASTERR_SIZE, "cannot add procs tid = %"PRIu64", parenttid = %"PRIi32", dirname = %s", tid, parenttid, procdirname);
				break;
			}

			if(tid_to_scan != -1)
			{
				//
				// procinfo should be filled, except when
				// the main thread already terminated and
				// the various proc files were not readable
				//
				// ASSERT(*procinfo);
				break;
			}
		}

		//
		// See if this process includes tasks that need to be added
		//
		snprintf(childdir, sizeof(childdir), "%s/%u/task", procdirname, (int)tid);
		if(scap_proc_scan_proc_dir(handle, childdir, tid, tid_to_scan, procinfo, error) == SCAP_FAILURE)
		{
			res = SCAP_FAILURE;
			break;
		}

		if(tid_to_scan != -1 && *procinfo)
		{
			//
			// We found the process we were looking for, no need to keep iterating
			//
			break;
		}
	}

	closedir(dir_p);
	scap_fd_free_table(handle, &sockets);
	return res;
}

#endif // _WIN32

//
// Remove an entry from the process list by parsing a PPME_PROC_EXIT event
//
/*
void scap_proc_schedule_removal(scap_t* handle, scap_evt* pevent)
{
	struct scap_threadinfo* tinfo;
	uint64_t tid = scap_event_get_tid(pevent);

	//
	// We don't validate the event type (other than with an assertion) for performance reasons.
	// We assume that the caller will do the right thing.
	//
	ASSERT(pevent->type == PPME_PROC_X);

//printf("--- %u (tot:%u)\n", tid, HASH_COUNT(handle->m_proclist));

	//
	// Find the process
	//
	HASH_FIND_INT64(handle->m_proclist, &tid, tinfo);
	if(tinfo != NULL)
	{
		//
		// Process found, do the deletion
		//
		handle->m_proc_to_delete = tinfo;
	}
	else
	{
		//
		// The process is not in the table??
		// We have an assertion here to catch the problem in debug mode, but we don't want to treat this as an error,
		// we will just keep going and nothing bad will happen.
		//
//fprintf(stderr, "--- %llu (tot:%u)\n", tid, HASH_COUNT(handle->m_proclist));
		ASSERT(false);
	}

//printf("$$$ count:%u\n", HASH_COUNT(handle->m_proclist));
}
*/

//
// Delete a process entry
//
inline void scap_proc_delete(scap_t* handle, scap_threadinfo* proc)
{
	//
	// First, free the fd table for this process descriptor
	//
	scap_fd_free_proc_fd_table(handle, proc);

	//
	// Second, remove the process descriptor from the table
	//
	HASH_DEL(handle->m_proclist, proc);

	//
	// Third, free the memory
	//
	free(proc);
}

//
// Remove the process that was scheduled for deletion for this handle
//
/*
void scap_proc_remove_scheduled(scap_t* handle)
{
	//
	// No validation, just assertion. We assume that the caller knows what he's doing.
	//
	ASSERT(handle->m_proc_to_delete != NULL);

	scap_proc_delete(handle, handle->m_proc_to_delete);
	handle->m_proc_to_delete = NULL;
}
*/

//
// Free the process table
//
void scap_proc_free_table(scap_t* handle)
{
	struct scap_threadinfo* tinfo;
	struct scap_threadinfo* ttinfo;

	HASH_ITER(hh, handle->m_proclist, tinfo, ttinfo)
	{
		scap_proc_delete(handle, tinfo);
	}
}

/*
//
// Given an event, get the info entry for the process that generated it
//
struct scap_threadinfo* scap_proc_get_from_event(scap_t* handle, scap_evt* pevent)
{
	struct scap_threadinfo* tinfo;
	int64_t tid = scap_event_get_tid(pevent);

	HASH_FIND_INT64(handle->m_proclist, &tid, tinfo);
	if(tinfo == NULL)
	{
		ASSERT(false);
		snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "can't find process %"PRIu64, tid);
		return NULL;
	}

	return tinfo;
}

//
// Return the process info entry geiven a tid
//
struct scap_threadinfo* scap_proc_get_from_tid(scap_t* handle, int64_t tid)
{
	struct scap_threadinfo* tinfo;

	HASH_FIND_INT64(handle->m_proclist, &tid, tinfo);
	if(tinfo == NULL)
	{
		ASSERT(false);
		snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "can't find process %"PRIu64, tid);
		return NULL;
	}

	return tinfo;
}
*/

struct scap_threadinfo* scap_proc_get(scap_t* handle, int64_t tid)
{
#ifdef _WIN32
	return NULL;
#else
	struct scap_threadinfo* tinfo = NULL;

	if(scap_proc_scan_proc_dir(handle, "/proc", -1, tid, &tinfo, handle->m_lasterr) != SCAP_SUCCESS)
	{
		return NULL;
	}

	return tinfo;
#endif // WIN32
}

void scap_proc_free(scap_t* handle, struct scap_threadinfo* proc)
{
	scap_fd_free_proc_fd_table(handle, proc);
	free(proc);
}

//
// Internal helper functions to output the process table to screen
//
void scap_proc_print_info(scap_threadinfo* tinfo)
{
	fprintf(stderr, "TID:%"PRIu64" PID:%"PRIu64" FLAGS:%"PRIu32" COMM:%s EXE:%s ARGS:%s CWD:%s FLIMIT:%" PRId64 "\n", tinfo->tid, tinfo->pid, tinfo->flags,tinfo->comm, tinfo->exe, tinfo->args, tinfo->cwd, tinfo->fdlimit);
	scap_fd_print_table(tinfo);
}

void scap_proc_print_proc_by_tid(scap_t* handle, uint64_t tid)
{
	scap_threadinfo* tinfo;
	scap_threadinfo* ttinfo;

	HASH_ITER(hh, handle->m_proclist, tinfo, ttinfo)
	{
		if (tinfo->tid == tid)
		{
			scap_proc_print_info(tinfo);
		}
	}
}

void scap_proc_print_table(scap_t* handle)
{
	scap_threadinfo* tinfo;
	scap_threadinfo* ttinfo;

	printf("************** PROCESS TABLE **************\n");

	HASH_ITER(hh, handle->m_proclist, tinfo, ttinfo)
	{
		scap_proc_print_info(tinfo);
	}

	printf("*******************************************\n");
}
