#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
using namespace google::protobuf::io;

#include "../../driver/ppm_ringbuffer.h"
#include "sinsp.h"
#include "sinsp_int.h"
#include "metrics.h"
#include "draios.pb.h"

///////////////////////////////////////////////////////////////////////////////
// sinsp_counter_time implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_counter_time::sinsp_counter_time()
{
	clear();
}

void sinsp_counter_time::add(uint32_t cnt_delta, uint64_t time_delta)
{
	ASSERT(cnt_delta <= 1);

	m_count += cnt_delta;
	m_time_ns += time_delta;
}

void sinsp_counter_time::add(sinsp_counter_time* other)
{
	m_count += other->m_count;
	m_time_ns += other->m_time_ns;
}

void sinsp_counter_time::add(sinsp_counter_time_bytes* other)
{
	m_count += (other->m_count_in + other->m_count_out + other->m_count_other);
	m_time_ns += (other->m_time_ns_in + other->m_time_ns_out + other->m_time_ns_other);
}

void sinsp_counter_time::add(sinsp_counter_time_bidirectional* other)
{
	m_count += (other->m_count_in + other->m_count_out + other->m_count_other);
	m_time_ns += (other->m_time_ns_in + other->m_time_ns_out + other->m_time_ns_other);
}

void sinsp_counter_time::clear()
{
	m_count = 0;
	m_time_ns = 0;
}

void sinsp_counter_time::subtract(uint32_t cnt_delta, uint64_t time_delta)
{
	ASSERT(cnt_delta <= 1);

	if((cnt_delta > m_count) || (time_delta > m_time_ns))
	{
		ASSERT(false);
		m_count = 0;
		m_time_ns = 0;
		return;
	}

	m_count -= cnt_delta;
	m_time_ns -= time_delta;
}

void sinsp_counter_time::to_protobuf(draiosproto::counter_time* protobuf_msg, double time_normaliztion_factor)
{
	protobuf_msg->set_time_ns((uint64_t)(((double)m_time_ns) * time_normaliztion_factor));
	protobuf_msg->set_count(m_count);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_counter_time_bidirectional implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_counter_time_bidirectional::sinsp_counter_time_bidirectional()
{
	clear();
}

void sinsp_counter_time_bidirectional::add_in(uint32_t cnt_delta, uint64_t time_delta)
{
	ASSERT(cnt_delta <= 1);

	m_count_in += cnt_delta;
	m_time_ns_in += time_delta;
}

void sinsp_counter_time_bidirectional::add_out(uint32_t cnt_delta, uint64_t time_delta)
{
	ASSERT(cnt_delta <= 1);

	m_count_out += cnt_delta;
	m_time_ns_out += time_delta;
}

void sinsp_counter_time_bidirectional::add_other(uint32_t cnt_delta, uint64_t time_delta)
{
	ASSERT(cnt_delta <= 1);

	m_count_other += cnt_delta;
	m_time_ns_other += time_delta;
}

void sinsp_counter_time_bidirectional::add(sinsp_counter_time_bidirectional* other)
{
	m_count_in += other->m_count_in;
	m_count_out += other->m_count_out;
	m_count_other += other->m_count_other;
	m_time_ns_in += other->m_time_ns_in;
	m_time_ns_out += other->m_time_ns_out;
	m_time_ns_other += other->m_time_ns_other;
}

void sinsp_counter_time_bidirectional::clear()
{
	m_count_in = 0;
	m_count_out = 0;
	m_count_other = 0;
	m_time_ns_in = 0;
	m_time_ns_out = 0;
	m_time_ns_other = 0;
}

void sinsp_counter_time_bidirectional::to_protobuf(draiosproto::counter_time_bidirectional* protobuf_msg)
{
	protobuf_msg->set_time_ns_in(m_time_ns_in);
	protobuf_msg->set_time_ns_out(m_time_ns_out);
	protobuf_msg->set_count_in(m_count_in);
	protobuf_msg->set_count_out(m_count_out);
	// NOTE: other is not included because we don't need it in the sample, just for internal use
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_counter_bytes implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_counter_bytes::sinsp_counter_bytes()
{
	clear();
}

void sinsp_counter_bytes::add_in(uint32_t cnt_delta, uint32_t bytes_delta)
{
	ASSERT(cnt_delta <= 1);

	m_count_in += cnt_delta;
	m_bytes_in += bytes_delta;
}

void sinsp_counter_bytes::add_out(uint32_t cnt_delta, uint32_t bytes_delta)
{
	ASSERT(cnt_delta <= 1);

	m_count_out += cnt_delta;
	m_bytes_out += bytes_delta;
}

void sinsp_counter_bytes::add(sinsp_counter_bytes* other)
{
	m_count_in += other->m_count_in;
	m_count_out += other->m_count_out;
	m_bytes_in += other->m_bytes_in;
	m_bytes_out += other->m_bytes_out;
}

void sinsp_counter_bytes::clear()
{
	m_count_in = 0;
	m_count_out = 0;
	m_bytes_in = 0;
	m_bytes_out = 0;
}

void sinsp_counter_bytes::to_protobuf(draiosproto::counter_bytes* protobuf_msg)
{
	protobuf_msg->set_bytes_in(m_bytes_in);
	protobuf_msg->set_bytes_out(m_bytes_out);
	protobuf_msg->set_count_in(m_count_in);
	protobuf_msg->set_count_out(m_count_out);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_counter_time_bytes implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_counter_time_bytes::sinsp_counter_time_bytes()
{
	clear();
}

void sinsp_counter_time_bytes::add_in(uint32_t cnt_delta, uint64_t time_delta, uint32_t bytes_delta)
{
	m_count_in += cnt_delta;
	m_time_ns_in += time_delta;
	m_bytes_in += bytes_delta;
}

void sinsp_counter_time_bytes::add_out(uint32_t cnt_delta, uint64_t time_delta, uint32_t bytes_delta)
{
	m_count_out += cnt_delta;
	m_time_ns_out += time_delta;
	m_bytes_out += bytes_delta;
}

void sinsp_counter_time_bytes::add_other(uint32_t cnt_delta, uint64_t time_delta, uint32_t bytes_delta)
{
	m_count_other += cnt_delta;
	m_time_ns_other += time_delta;
	m_bytes_other += bytes_delta;
}

void sinsp_counter_time_bytes::add(sinsp_counter_time_bytes* other)
{
	m_count_in += other->m_count_in;
	m_count_out += other->m_count_out;
	m_count_other += other->m_count_other;
	m_time_ns_in += other->m_time_ns_in;
	m_time_ns_out += other->m_time_ns_out;
	m_time_ns_other += other->m_time_ns_other;
	m_bytes_in += other->m_bytes_in;
	m_bytes_out += other->m_bytes_out;
	m_bytes_other += other->m_bytes_other;
}

void sinsp_counter_time_bytes::add(sinsp_counter_time_bidirectional* other, bool time_only)
{
	if(!time_only)
	{
		m_count_in += other->m_count_in;
		m_count_out += other->m_count_out;
		m_count_other += other->m_count_other;
	}

	m_time_ns_in += other->m_time_ns_in;
	m_time_ns_out += other->m_time_ns_out;
	m_time_ns_other += other->m_time_ns_other;
}

void sinsp_counter_time_bytes::add(sinsp_counter_time* other)
{
	m_count_other += other->m_count;
	m_time_ns_other += other->m_time_ns;
}

void sinsp_counter_time_bytes::clear()
{
	m_count_in = 0;
	m_count_out = 0;
	m_count_other = 0;
	m_time_ns_in = 0;
	m_time_ns_out = 0;
	m_time_ns_other = 0;
	m_bytes_in = 0;
	m_bytes_out = 0;
	m_bytes_other = 0;
}

void sinsp_counter_time_bytes::to_protobuf(draiosproto::counter_time_bytes* protobuf_msg, double time_normaliztion_factor)
{
	protobuf_msg->set_time_ns_in((uint64_t)(((double)m_time_ns_in) * time_normaliztion_factor));
	protobuf_msg->set_time_ns_out((uint64_t)(((double)m_time_ns_out) * time_normaliztion_factor));
	protobuf_msg->set_time_ns_other((uint64_t)(((double)m_time_ns_other) * time_normaliztion_factor));
	protobuf_msg->set_count_in(m_count_in);
	protobuf_msg->set_count_out(m_count_out);
	protobuf_msg->set_count_other(m_count_other);
	protobuf_msg->set_bytes_in(m_bytes_in);
	protobuf_msg->set_bytes_out(m_bytes_out);
	protobuf_msg->set_bytes_other(m_bytes_other);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_counters implementation
///////////////////////////////////////////////////////////////////////////////
void sinsp_counters::clear()
{
	m_unknown.clear();
	m_other.clear();
	m_file.clear();
	m_net.clear();
	m_ipc.clear();
	m_memory.clear();
	m_process.clear();
	m_sleep.clear();
	m_system.clear();
	m_signal.clear();
	m_user.clear();
	m_time.clear();
	m_io_file.clear();
	m_io_net.clear();
	m_io_other.clear();
	m_wait_file.clear();
	m_wait_net.clear();
	m_wait_ipc.clear();
	m_wait_other.clear();
	m_processing.clear();

	m_nthreads = 0;
}

void sinsp_counters::get_total(sinsp_counter_time* tot)
{
	tot->add(&m_unknown);
	tot->add(&m_other);
	tot->add(&m_file);
	tot->add(&m_net);
	tot->add(&m_ipc);
	tot->add(&m_memory);
	tot->add(&m_process);
	tot->add(&m_sleep);
	tot->add(&m_system);
	tot->add(&m_signal);
	tot->add(&m_user);
	tot->add(&m_time);
	tot->add(&m_io_file);
	tot->add(&m_io_net);
	tot->add(&m_io_other);
	tot->add(&m_wait_file);
	tot->add(&m_wait_net);
	tot->add(&m_wait_ipc);
	tot->add(&m_wait_other);
	tot->add(&m_processing);
}

void sinsp_counters::add(sinsp_counters* other)
{
	m_unknown.add(&other->m_unknown);
	m_other.add(&other->m_other);
	m_file.add(&other->m_file);
	m_net.add(&other->m_net);
	m_ipc.add(&other->m_ipc);
	m_memory.add(&other->m_memory);
	m_process.add(&other->m_process);
	m_sleep.add(&other->m_sleep);
	m_system.add(&other->m_system);
	m_signal.add(&other->m_signal);
	m_user.add(&other->m_user);
	m_time.add(&other->m_time);
	m_io_file.add(&other->m_io_file);
	m_io_net.add(&other->m_io_net);
	m_io_other.add(&other->m_io_other);
	m_wait_file.add(&other->m_wait_file);
	m_wait_net.add(&other->m_wait_net);
	m_wait_ipc.add(&other->m_wait_ipc);
	m_wait_other.add(&other->m_wait_other);
	m_processing.add(&other->m_processing);

	m_nthreads++;
}

void sinsp_counters::to_protobuf(draiosproto::time_categories* protobuf_msg, uint64_t sample_length_ns)
{
	m_tot_other.clear();
	// Unknown is usually garbage caused by drops or processes coming out of
	// inactivity. We just ignore it.
	//m_tot_other.add(&m_unknown);
	m_tot_other.add(&m_other);
	m_tot_other.add(&m_memory);
	m_tot_other.add(&m_process);
	m_tot_other.add(&m_system);
	m_tot_other.add(&m_signal);
	m_tot_other.add(&m_user);
	m_tot_other.add(&m_time);
	m_tot_other.add(&m_io_other);

	m_tot_wait.clear();
	m_tot_wait.add(&m_wait_other);
	m_tot_wait.add(&m_sleep);

	m_tot_io_file.clear();
	m_tot_io_file.add(&m_file);
	m_tot_io_file.add(&m_io_file);
	m_tot_io_file.add(&m_wait_file, true);

	m_tot_io_net.clear();
	m_tot_io_net.add(&m_net);
	m_tot_io_net.add(&m_io_net);
	m_tot_io_net.add(&m_wait_net, true);

	m_tot_ipc.clear();
	m_tot_ipc.add(&m_ipc);
	m_tot_ipc.add(&m_wait_ipc);

	m_tot_relevant.clear();
	m_tot_relevant.add(&m_tot_other);
	m_tot_relevant.add(&m_tot_io_file);
	m_tot_relevant.add(&m_tot_io_net);
	m_tot_relevant.add(&m_processing);

	m_tot_other.to_protobuf(protobuf_msg->mutable_other(), ((double)1000000000) / m_tot_relevant.m_time_ns);
	m_tot_io_file.to_protobuf(protobuf_msg->mutable_io_file(), ((double)1000000000) / m_tot_relevant.m_time_ns);
	m_tot_io_net.to_protobuf(protobuf_msg->mutable_io_net(), ((double)1000000000) / m_tot_relevant.m_time_ns);
	m_processing.to_protobuf(protobuf_msg->mutable_processing(), ((double)1000000000) / m_tot_relevant.m_time_ns);

#ifdef _DEBUG
	sinsp_counter_time ttot;
	ttot.add(&m_tot_other);
	ttot.add(&m_tot_wait);
	ttot.add(&m_tot_io_file);
	ttot.add(&m_tot_io_net);
	ttot.add(&m_tot_ipc);
	ttot.add(&m_processing);
	ttot.add(&m_unknown);
	ASSERT(ttot.m_time_ns % 1000000000 == 0);
	ASSERT(ttot.m_time_ns / 1000000000 == m_nthreads);
#endif
}

uint64_t sinsp_counters::get_total_other_time()
{
	return m_tot_other.m_time_ns;
}

uint64_t sinsp_counters::get_total_wait_time()
{
	return m_tot_wait.m_time_ns;
}

uint64_t sinsp_counters::get_total_file_time()
{
	return m_tot_io_file.m_time_ns_in + m_tot_io_file.m_time_ns_out + m_tot_io_file.m_time_ns_other;
}

uint64_t sinsp_counters::get_total_net_time()
{
	return m_tot_io_net.m_time_ns_in + m_tot_io_net.m_time_ns_out + m_tot_io_net.m_time_ns_other;
}

uint64_t sinsp_counters::get_total_ipc_time()
{
	return m_tot_ipc.m_time_ns;
}

double sinsp_counters::get_processing_percentage()
{
	return ((double)m_processing.m_time_ns) / m_tot_relevant.m_time_ns;
}

double sinsp_counters::get_file_percentage()
{
	return ((double)m_tot_io_file.m_time_ns_in + m_tot_io_file.m_time_ns_out + m_tot_io_file.m_time_ns_other) / m_tot_relevant.m_time_ns;
}

double sinsp_counters::get_net_percentage()
{
	return ((double)m_tot_io_net.m_time_ns_in + m_tot_io_net.m_time_ns_out + m_tot_io_net.m_time_ns_other) / m_tot_relevant.m_time_ns;
}

double sinsp_counters::get_other_percentage()
{
	return ((double)m_tot_other.m_time_ns) / m_tot_relevant.m_time_ns;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_transaction_counters implementation
///////////////////////////////////////////////////////////////////////////////
void sinsp_transaction_counters::clear()
{
	m_counter.clear();
}

void sinsp_transaction_counters::to_protobuf(draiosproto::counter_time_bidirectional* protobuf_msg)
{
	m_counter.to_protobuf(protobuf_msg);
}

void sinsp_transaction_counters::add(sinsp_transaction_counters* other)
{
	m_counter.add(&other->m_counter);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_connection_counters implementation
///////////////////////////////////////////////////////////////////////////////
void sinsp_connection_counters::clear()
{
	m_server.clear();
	m_client.clear();
}

void sinsp_connection_counters::to_protobuf(draiosproto::connection_categories* protobuf_msg)
{
	m_server.to_protobuf(protobuf_msg->mutable_server());
	m_client.to_protobuf(protobuf_msg->mutable_client());
}

void sinsp_connection_counters::add(sinsp_connection_counters* other)
{
	m_server.add(&other->m_server);
	m_client.add(&other->m_client);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_error_counters implementation
///////////////////////////////////////////////////////////////////////////////
void sinsp_error_counters::clear()
{
	m_table.clear();
}

void sinsp_error_counters::to_protobuf(draiosproto::counter_syscall_errors* protobuf_msg)
{
	protobuf_msg->set_count(m_table.size());

	map<int32_t, sinsp_counter_cnt>::iterator it;

	uint32_t j;

	for(it = m_table.begin(), j = 0; it != m_table.end(); ++it, j++)
	{
		if(j >= MAX_N_ERROR_CODES_IN_PROTO)
		{
			return;
		}

		protobuf_msg->add_top_error_codes(it->first);
	}
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_host_metrics implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_host_metrics::sinsp_host_metrics()
{
	clear();
}

void sinsp_host_metrics::clear()
{
	m_metrics.clear();
	m_transaction_metrics.clear();
	m_transaction_processing_delay_ns = 0;
	m_capacity_score = -1;
	m_n_capacity_score_entries = 0;
	m_connection_queue_usage_pct = 0;
	m_fd_usage_pct = 0;
	m_syscall_errors.clear();
}

void sinsp_host_metrics::add(sinsp_procinfo* pinfo)
{
	m_metrics.add(&pinfo->m_proc_metrics);
	m_transaction_metrics.add(&pinfo->m_proc_transaction_metrics);
	m_transaction_processing_delay_ns += pinfo->m_proc_transaction_processing_delay_ns;

	if(pinfo->m_connection_queue_usage_pct > m_connection_queue_usage_pct)
	{
		m_connection_queue_usage_pct = pinfo->m_connection_queue_usage_pct;
	}

	if(pinfo->m_fd_usage_pct > m_fd_usage_pct)
	{
		m_fd_usage_pct = pinfo->m_fd_usage_pct;
	}
}
