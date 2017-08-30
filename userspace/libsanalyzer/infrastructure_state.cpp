#include <algorithm>

#include "infrastructure_state.h"

bool get_cached_result(infrastructure_state::policy_cache_t &cache, std::string &id, uint64_t policy_id, bool *res)
{
	auto cached_results = cache.find(id);
	if(cached_results != cache.end()) {
		auto cached_result = cached_results->second.find(policy_id);
		if (cached_result != cached_results->second.end()) {
			//glogf(sinsp_logger::SEV_DEBUG, "Found cached result for id %s, policy id %llu --> %s", id.c_str(), policy_id, res?"true":"false");
			*res = cached_result->second;
			return true;
		}
	}

	//glogf(sinsp_logger::SEV_DEBUG, "Cannot find cached result for id %s, policy id %llu", id.c_str(), policy_id);

	return false;
}

void insert_cached_result(infrastructure_state::policy_cache_t &cache, std::string &id, uint64_t policy_id, bool res)
{
	if(cache.find(id) == cache.end()) {
		cache.emplace(id, std::unordered_map<uint64_t, bool>());
	}

	cache[id].emplace(policy_id, res);

	//glogf(sinsp_logger::SEV_DEBUG, "Cache result (%s) for id %s, policy id %llu", res?"true":"false", id.c_str(), policy_id);
}

bool evaluate_on(draiosproto::container_group *congroup, google::protobuf::RepeatedPtrField<draiosproto::scope_predicate> &preds)
{
	auto evaluate = [](const draiosproto::scope_predicate &p, const std::string &value)
	{
		// KISS for now
		glogf(sinsp_logger::SEV_DEBUG, "Evaluating %s %s %s%s with value %s", p.key().c_str(), draiosproto::scope_operator_Name(p.op()).c_str(), p.values(0).c_str(),
			((p.op() == draiosproto::IN_SET || p.op() == draiosproto::NOT_IN_SET)?"...":""), value.c_str());
		bool ret;
		switch(p.op()) {
		case draiosproto::EQ:
			ret = p.values(0) == value;
			break;
		case draiosproto::NOT_EQ:
			ret = p.values(0) != value;
			break;
		case draiosproto::CONTAINS:
			ret = value.find(p.values(0)) != std::string::npos;
			break;
		case draiosproto::NOT_CONTAINS:
			ret = value.find(p.values(0)) == std::string::npos;
			break;
		case draiosproto::STARTS_WITH:
			ret = value.substr(0, p.values(0).size()) == p.values(0);
			break;
		case draiosproto::IN_SET:
			ret = false;
			for(auto v : p.values()) {
				if (v == value) {
					ret = true;
					break;
				}
			}
			break;
		case draiosproto::NOT_IN_SET:
			ret = true;
			for(auto v : p.values()) {
				if (v == value) {
					ret = false;
					break;
				}
			}
			break;
		default:
			glogf(sinsp_logger::SEV_WARNING, "Cannot evaluated scope_predicate %s", p.DebugString().c_str());
			ret = true;
		}

		return ret;
	};

	for(auto i = preds.begin(); i != preds.end();) {
		if(congroup->tags().find(i->key()) != congroup->tags().end()) {
			if(!evaluate(*i, congroup->tags().at(i->key()))) {
				preds.erase(i);
				return false;
			} else {
				i = preds.erase(i);
			}
		} else {
			++i;
		}
	}

	return true;
}

infrastructure_state::infrastructure_state(uint64_t refresh_interval) :
	m_k8s_subscribed(false),
	m_k8s_interval(refresh_interval)
{
	m_k8s_callback = [this] (bool successful, google::protobuf::Message *response_msg) {

		if(successful) {
			draiosproto::congroup_update_event *evt = (draiosproto::congroup_update_event *)response_msg;
			handle_event(evt);
		} else {
			//
			// Error from cointerface, destroy the whole state and subscribe again
			//
			glogf(sinsp_logger::SEV_WARNING, "Error while receiving k8s orchestrator events. Reset and retry.");
			reset();
		}
	};
}

void infrastructure_state::init(sinsp *inspector, const std::string& machine_id)
{
	m_inspector = inspector;
	m_machine_id = machine_id;
}

infrastructure_state::~infrastructure_state(){}

void infrastructure_state::subscribe_to_k8s(const string& url)
{
	m_k8s_url = url;
	glogf(sinsp_logger::SEV_DEBUG, "Subscribe to k8s orchestrator events.");
	sdc_internal::orchestrator_events_stream_command cmd;
	cmd.set_url(m_k8s_url);
	m_k8s_subscribed = true;
	m_k8s_coclient.get_orchestrator_events(cmd, m_k8s_callback);
}

bool infrastructure_state::subscribed()
{
	return m_k8s_subscribed; // || m_mesos_subscribed || ...
}

void infrastructure_state::refresh(uint64_t ts)
{
	if (m_k8s_subscribed) {
		m_k8s_interval.run([this]()
		{
			m_k8s_coclient.next();
		}, ts);
	}

	// if (m_mesos_subscribed) { ... }

	//
	// Calling empty before locking to avoid useless overhead
	//
	if(!m_host_events_queue.empty() && m_host_events_queue_mutex.try_lock()) {
		glogf(sinsp_logger::SEV_DEBUG, "Hosts metadata available and lock aquired. Start refresh operation of %d hosts.", m_host_events_queue.size());
		refresh_hosts_metadata();
		m_host_events_queue_mutex.unlock();
		glogf(sinsp_logger::SEV_DEBUG, "Refresh of hosts metadata completed and lock unlocked.");
	}
}

void infrastructure_state::reset()
{
	m_container_p_cache.clear();
	m_host_p_cache.clear();
	m_orphans.clear();
	m_state.clear();
	m_k8s_cached_cluster_id.clear();

	if (m_k8s_subscribed) {
		subscribe_to_k8s(m_k8s_url);
	}
}

void infrastructure_state::load_single_event(const draiosproto::congroup_update_event &evt, bool overwrite)
{
	handle_event(&evt, overwrite);
}

unsigned int infrastructure_state::size()
{
	return m_state.size();
}

bool infrastructure_state::has(uid_t uid)
{
	return m_state.find(uid) != m_state.end();
}

std::unique_ptr<draiosproto::container_group> infrastructure_state::get(uid_t uid)
{
	if(!has(uid)) {
		return nullptr;
	}

	auto res = make_unique<draiosproto::container_group>();
	res->CopyFrom(*m_state[uid]);

	return res;
}

void infrastructure_state::handle_event(const draiosproto::congroup_update_event *evt, bool overwrite)
{
	std::string kind = evt->object().uid().kind();
	std::string id = evt->object().uid().id();

	glogf(sinsp_logger::SEV_DEBUG, "Handling %s event with uid <%s,%s>", draiosproto::congroup_event_type_Name(evt->type()).c_str(), kind.c_str(), id.c_str());
	auto key = make_pair(kind, id);

	if(!has(key)) {
		switch(evt->type()) {
		case draiosproto::ADDED:
			m_state[key] = make_unique<draiosproto::container_group>();
			m_state[key]->CopyFrom(evt->object());
			connect(key);
			glogf(sinsp_logger::SEV_DEBUG, m_state[key]->DebugString().c_str());
			break;
		case draiosproto::REMOVED:
			// allow double delete (example: remove a container for an already terminated k8s_job)
			glogf(sinsp_logger::SEV_DEBUG, "Ignoring request to delete non-existent container group <%s,%s>", kind.c_str(), id.c_str());
			break;
		case draiosproto::UPDATED:
			glogf(sinsp_logger::SEV_WARNING, "Ignoring request to update container_group <%s,%s> because it does not exists.", kind.c_str(), id.c_str());
			break;
		}
	} else {
		switch(evt->type()) {
		case draiosproto::ADDED:
			if (!overwrite) {
				if(kind != "container") {
					glogf(sinsp_logger::SEV_WARNING, "Cannot add container_group <%s,%s> because it's already present.", kind.c_str(), id.c_str());
				}
				break;
			}
			glogf(sinsp_logger::SEV_DEBUG, "Overwrite container group <%s,%s>", kind.c_str(), id.c_str());
			m_state[key]->CopyFrom(evt->object());
			glogf(sinsp_logger::SEV_DEBUG, m_state[key]->DebugString().c_str());
			break;
		case draiosproto::REMOVED:
			glogf(sinsp_logger::SEV_DEBUG, m_state[key]->DebugString().c_str());
			remove(key);
			break;
		case draiosproto::UPDATED:
			if(evt->object().parents().size() > 0 ||
			   evt->object().children().size() > 0 ||
			   evt->object().ports().size() > 0) {
				glogf(sinsp_logger::SEV_DEBUG, "UPDATED event will change relationships, remove the container group then connect it again");
				remove(key);
				m_state[key] = make_unique<draiosproto::container_group>();
				m_state[key]->CopyFrom(evt->object());
				connect(key);
			} else {
				glogf(sinsp_logger::SEV_DEBUG, "UPDATED event will not change relationships, just update the metadata");
				*m_state[key]->mutable_tags() = evt->object().tags();
				m_state[key]->mutable_ip_addresses()->CopyFrom(evt->object().ip_addresses());
				m_state[key]->mutable_metrics()->CopyFrom(evt->object().metrics());
			}
			glogf(sinsp_logger::SEV_DEBUG, m_state[key]->DebugString().c_str());
			break;
		}
	}

	glogf(sinsp_logger::SEV_DEBUG, "%s event with uid <%s,%s> handled. Current state size: %d", draiosproto::congroup_event_type_Name(evt->type()).c_str(), kind.c_str(), id.c_str(), m_state.size());
	if(g_logger.get_severity() >= sinsp_logger::SEV_TRACE) {
		debug_print();
	}
}

bool infrastructure_state::has_link(const google::protobuf::RepeatedPtrField<draiosproto::congroup_uid>& links, const uid_t& uid)
{
	for (const auto &l : links) {
		if(l.kind() == uid.first && l.id() == uid.second) {
			return true;
		}
	}

	return false;
}

void infrastructure_state::connect(infrastructure_state::uid_t& key)
{
	//
	// Connect the new group to his parents
	//
	for (const auto &x : m_state[key]->parents()) {
		auto pkey = make_pair(x.kind(), x.id());
		if(!has(pkey)) {
			// keep track of the missing parent. We will fix the children links when this event arrives
			if(m_orphans.find(pkey) == m_orphans.end())
				m_orphans[pkey] = std::vector<uid_t>();
			m_orphans[pkey].emplace_back(key.first, key.second);
		} else if(!has_link(m_state[pkey]->children(), key)) {
			draiosproto::congroup_uid *child = m_state[pkey]->mutable_children()->Add();
			child->set_kind(key.first);
			child->set_id(key.second);
			glogf(sinsp_logger::SEV_DEBUG, "child <%s,%s> added to <%s,%s>",
				  key.first.c_str(), key.second.c_str(), pkey.first.c_str(), pkey.second.c_str());
		} else {
			glogf(sinsp_logger::SEV_DEBUG, "<%s,%s> already connected to child <%s,%s>",
				pkey.first.c_str(), pkey.second.c_str(), key.first.c_str(), key.second.c_str());
		}
	}

	//
	// and connect his children to him
	//
	for (const auto &x : m_state[key]->children()) {
		auto ckey = make_pair(x.kind(), x.id());
		if(!has(ckey)) {
			// the connection will be created when the child arrives
			continue;
		} else if(!has_link(m_state[ckey]->parents(), key)) {
			draiosproto::congroup_uid *parent = m_state[ckey]->mutable_parents()->Add();
			parent->set_kind(key.first);
			parent->set_id(key.second);
			glogf(sinsp_logger::SEV_DEBUG, "parent <%s,%s> added to <%s,%s>",
				key.first.c_str(), key.second.c_str(), ckey.first.c_str(), ckey.second.c_str());
		} else {
			glogf(sinsp_logger::SEV_DEBUG, "<%s,%s> already connected to parent <%s,%s>",
				ckey.first.c_str(), ckey.second.c_str(), key.first.c_str(), key.second.c_str());
		}
	}

	// Fix any broken link involving this container group
	// do this after checking the children otherwise this node will be added as parent twice
	if(m_orphans.find(key) != m_orphans.end()) {
		for(const auto &orphan_uid : m_orphans[key]) {
			if(!has_link(m_state[key]->children(), orphan_uid)) {
				draiosproto::congroup_uid *child = m_state[key]->mutable_children()->Add();
				child->set_kind(orphan_uid.first);
				child->set_id(orphan_uid.second);
				glogf(sinsp_logger::SEV_DEBUG, "(deferred) child <%s,%s> added to <%s,%s>",
					orphan_uid.first.c_str(), orphan_uid.second.c_str(), key.first.c_str(), key.second.c_str());
			} else {
				glogf(sinsp_logger::SEV_DEBUG, "(deferred) <%s,%s> already connected to <%s,%s>",
					key.first.c_str(), key.second.c_str(), orphan_uid.first.c_str(), orphan_uid.second.c_str());
			}
		}
		m_orphans.erase(key);
	}
}

void infrastructure_state::remove(infrastructure_state::uid_t& key)
{
	//
	// Remove all children references to this group
	//
	glogf(sinsp_logger::SEV_DEBUG, "Remove container group <%s,%s>", key.first.c_str(), key.second.c_str());

	glogf(sinsp_logger::SEV_DEBUG, "Container group <%s,%s> has %d parents", key.first.c_str(), key.second.c_str(), m_state[key]->parents().size());
	for (const auto &x : m_state[key]->parents()) {
		auto pkey = make_pair(x.kind(), x.id());

		if(!has(pkey)) {
			// parent has already been deleted
			glogf(sinsp_logger::SEV_DEBUG, "Container group <%s,%s> has been already deleted", pkey.first.c_str(), pkey.second.c_str());
			continue;
		}

		bool erased = false;
		glogf(sinsp_logger::SEV_DEBUG, "Searching children links inside container group <%s,%s>", pkey.first.c_str(), pkey.second.c_str());

		for (auto pos = m_state[pkey]->children().begin(); pos != m_state[pkey]->children().end();) {
			if (pos->kind() == key.first && pos->id() == key.second) {
				glogf(sinsp_logger::SEV_DEBUG, "Erase child link from <%s,%s>", pkey.first.c_str(), pkey.second.c_str());
				m_state[pkey]->mutable_children()->erase(pos);
				glogf(sinsp_logger::SEV_DEBUG, "Child link erased.");
				erased = true;
				break;
			} else {
				++pos;
			}
		}

		if (!erased) {
			glogf(sinsp_logger::SEV_WARNING, "Container groups inconsistency detected. <%s,%s> should be a child of <%s,%s>.",
				m_state[key]->uid().kind().c_str(), m_state[key]->uid().id().c_str(), m_state[pkey]->uid().kind().c_str(), m_state[pkey]->uid().id().c_str());
		}
	}

	if (m_state[key]->uid().kind() == "container") {
		//
		// Delete all cached results for this container
		//
		m_container_p_cache.erase(m_state[key]->uid().id());
	}

	// Remove the group itself
	m_state.erase(key);

	glogf(sinsp_logger::SEV_DEBUG, "Container group <%s,%s> removed.", key.first.c_str(), key.second.c_str());
}

bool infrastructure_state::walk_and_match(draiosproto::container_group *congroup,
										google::protobuf::RepeatedPtrField<draiosproto::scope_predicate> &preds,
										std::unordered_set<uid_t> &visited_groups)
{
	uid_t uid = make_pair(congroup->uid().kind(), congroup->uid().id());

	if(visited_groups.find(uid) != visited_groups.end()) {
		// Group already visited, continue the evaluation
		return true;
	}

	//
	// Evaluate current group's fields
	// Remove the successfully evaluated ones
	//
	if(!evaluate_on(congroup, preds)) {
		// A predicate is false
		return false;
	}

	//
	// All predicates evalutated successfully,
	// nothing else to do
	//
	if (preds.empty()) return true;

	// Remember we've visited this group
	visited_groups.emplace(uid);

	//
	// Evaluate parents' tags
	//
	for(const auto &p_uid : congroup->parents()) {

		auto pkey = make_pair(p_uid.kind(), p_uid.id());

		if(!has(pkey)) {
			// We don't have this parent (yet...)
			glogf(sinsp_logger::SEV_WARNING, "Cannot fully evaluate policy scope because the infrastructure state is incomplete.");
			return false;
		}

		if(!walk_and_match(m_state[pkey].get(), preds, visited_groups)) {
			// A predicate in the upper levels returned false
			// The final result is false
			return false;
		}
		if (preds.empty()) break;
	}

	return true;
}

bool infrastructure_state::match_scope(std::string &container_id, std::string &host_id, const draiosproto::policy &policy)
{
	// glogf(sinsp_logger::SEV_DEBUG, "Match policy scope with c_id: \"%s\", h_id: \"%s\", p_id: %llu, container_scope: %s, host_scope: %s",
	// 	container_id.c_str(), host_id.c_str(), policy.id(), policy.container_scope()?"true":"false", policy.host_scope()?"true":"false");

	bool result = true;
	uid_t uid;

	if((container_id.empty() && !policy.host_scope()) ||
		(!container_id.empty() && !policy.container_scope())) {
		// the policy isn't meant to be applied to this event
		return false;
	}

	if (!container_id.empty() && policy.container_scope()) {
		uid = make_pair("container", container_id);
	} else {
		uid = make_pair("host", host_id);
	}

	policy_cache_t &cache = uid.first == "host" ? m_host_p_cache : m_container_p_cache;

	if(policy.scope_predicates().empty()) {
		// no predicates, we can safely return true immediately
		result = true;
	} else {

		if(get_cached_result(cache, uid.second, policy.id(), &result)) {
			return result;
		}

		auto pos = m_state.find(uid);
		if (pos == m_state.end())
			return false;

		google::protobuf::RepeatedPtrField<draiosproto::scope_predicate> preds(policy.scope_predicates());

		if (uid.first == "host") {
			result = evaluate_on(pos->second.get(), preds);
		} else {
			std::unordered_set<uid_t, std::hash<uid_t>> visited;
			result = walk_and_match(pos->second.get(), preds, visited);
		}

		if (result && !preds.empty()) {
			glogf(sinsp_logger::SEV_DEBUG, "Predicates list not empty, check operators...");
			auto i = preds.begin();
			for(; i != preds.end(); ++i) {
				if(i->op() != draiosproto::NOT_EQ && i->op() != draiosproto::NOT_IN_SET && i->op() != draiosproto::NOT_CONTAINS) {
					break;
				}
			}
			if (i == preds.end()) {
				glogf(sinsp_logger::SEV_DEBUG, "The unmatched predicates are only !=, not in, not contains. Assume the metrics are not set in the current sub-infrastructure and apply the policy");
				result = true;
			} else {
				result = false;
			}
		}

		glogf(sinsp_logger::SEV_DEBUG, "Matching policy %llu, composed by %d predicates, against <%s,%s> ----> %s", policy.id(), policy.scope_predicates().size(), uid.first.c_str(), uid.second.c_str(), (result?"true":"false"));
	}

	insert_cached_result(cache, uid.second, policy.id(), result);

	return result;
}

void infrastructure_state::state_of(const draiosproto::container_group *grp,
				     google::protobuf::RepeatedPtrField<draiosproto::container_group>* state,
				     std::unordered_set<uid_t>& visited)
{
	uid_t uid = make_pair(grp->uid().kind(), grp->uid().id());

	if(visited.find(uid) != visited.end()) {
		// Group already visited, skip it
		return;
	}
	visited.emplace(uid);


	for (const auto &p_uid : grp->parents()) {
		auto pkey = make_pair(p_uid.kind(), p_uid.id());

		if(!has(pkey)) {
			// We don't have this parent (yet...)
			continue;
		}

		//
		// Build parent state
		//
		state_of(m_state[pkey].get(), state, visited);
	}

	//
	// Except for containers and hosts, add the current node
	//
	if(grp->uid().kind() != "container" && grp->uid().kind() != "host") {
		auto x = state->Add();
		x->CopyFrom(*grp);
		x->mutable_metrics()->erase(x->mutable_metrics()->begin(), x->mutable_metrics()->end());
		// Clean children links, backend will reconstruct them from parent ones
		if(grp->uid().kind() != "k8s_pod")
		{
			x->mutable_children()->Clear();
		}
		// Put back legacy metrics
		auto add_metric_if_found = [grp](const string& metric_name, draiosproto::container_group* dest)
		{
			auto it = find_if(grp->metrics().cbegin(), grp->metrics().cend(), [&metric_name](const draiosproto::app_metric& m)
			{
				return m.name() == metric_name;
			});
			if(it != grp->metrics().cend())
			{
				dest->mutable_metrics()->Add()->CopyFrom(*it);
			}
		};

		if(x->uid().kind() == "k8s_pod")
		{
			add_metric_if_found("kubernetes.pod.container.status.restarts", x);
		}
		else if(x->uid().kind() == "k8s_replicaset")
		{
			add_metric_if_found("kubernetes.replicaset.status.replicas", x);
			add_metric_if_found("kubernetes.replicaset.spec.replicas", x);
		}
		else if(x->uid().kind() == "k8s_replicationcontroller")
		{
			add_metric_if_found("kubernetes.replicationcontroller.status.replicas", x);
			add_metric_if_found("kubernetes.replicationcontroller.spec.replicas", x);
		}
	}
}

void infrastructure_state::state_of(const std::vector<std::string> &container_ids, google::protobuf::RepeatedPtrField<draiosproto::container_group>* state)
{
	std::unordered_set<uid_t, std::hash<uid_t>> visited;

	//
	// Retrieve the state of every container
	//
	for(const auto &c_id : container_ids) {
		auto pos = m_state.find(make_pair("container", c_id));
		if (pos == m_state.end()) {
			//
			// This container is not in the orchestrator state
			//
			continue;
		}

		state_of(pos->second.get(), state, visited);
	}

	//
	// Clean up the broken links
	// (except for container links, that are used to identify the containers)
	//
	for(auto state_cgroup = state->begin(), state_end = state->end(); state_cgroup != state_end; ++state_cgroup) {
		for(auto i = state_cgroup->mutable_children()->begin(); i != state_cgroup->mutable_children()->end();) {
			if(i->kind() != "container" &&
			   visited.find(make_pair(i->kind(), i->id())) == visited.end()) {
				i = state_cgroup->mutable_children()->erase(i);
			} else {
				++i;
			}
		}
		// clean up the host link
		if(host_children.find(state_cgroup->uid().kind()) != host_children.end()) {
			for(auto i = state_cgroup->mutable_parents()->begin(), i_end = state_cgroup->mutable_parents()->end(); i != i_end; ++i) {
				if(i->kind() == "host") {
					state_cgroup->mutable_parents()->erase(i);
					break;
				}
			}
		}
	}
}

void infrastructure_state::get_state(google::protobuf::RepeatedPtrField<draiosproto::container_group>* state)
{
	for (auto i = m_state.begin(); i != m_state.end(); ++i) {
		auto cg = i->second.get();
		if(cg->uid().kind() != "container" && cg->uid().kind() != "host") {
			auto x = state->Add();
			x->CopyFrom(*cg);
			// clean up host links
			if(host_children.find(cg->uid().kind()) != host_children.end()) {
				for(auto j = x->mutable_parents()->begin(), j_end = x->mutable_parents()->end(); j != j_end; ++i) {
					if(j->kind() == "host") {
						x->mutable_parents()->erase(j);
						break;
					}
				}
			}
			// Clean children links, backend will reconstruct them from parent ones
			if(cg->uid().kind() != "k8s_pod")
			{
				x->mutable_children()->Clear();
			}
		}
	}
}

void infrastructure_state::on_new_container(const sinsp_container_info& container_info)
{
	if(container_info.m_name.find("k8s_POD") != std::string::npos) {
		// filter out k8s internal container/s
		return;
	}

	glogf(sinsp_logger::SEV_DEBUG, "Receiving new container event (id: %s) from container_manager", container_info.m_id.c_str());
	draiosproto::congroup_update_event evt;
	evt.set_type(draiosproto::ADDED);
	auto cg = evt.mutable_object();
	cg->mutable_uid()->set_kind("container");
	cg->mutable_uid()->set_id(container_info.m_id);
	(*cg->mutable_tags())["container.name"] = container_info.m_name;
	(*cg->mutable_tags())["container.image"] = container_info.m_image;
	(*cg->mutable_tags())["container.image.id"] = container_info.m_imageid.substr(0, 12);
	for (const auto &t : container_info.m_labels) {
		(*cg->mutable_tags())["container.label." + t.first] = t.second;
		if(m_k8s_subscribed && std::string(t.first) == "io.kubernetes.pod.uid") {
			auto p = cg->mutable_parents()->Add();
			p->set_kind("k8s_pod");
			p->set_id(t.second);
			glogf(sinsp_logger::SEV_DEBUG, "Adding parent <k8s_pod,%s> to container %s", t.second.c_str(), container_info.m_id.c_str());
		}
	}

	handle_event(&evt, true);
}

void infrastructure_state::on_remove_container(const sinsp_container_info& container_info)
{
	if(container_info.m_name.find("k8s_POD") != std::string::npos) {
		// filter out k8s internal container/s
		return;
	}

	glogf(sinsp_logger::SEV_DEBUG, "Receiving remove container event (id: %s) from container_manager", container_info.m_id.c_str());
	draiosproto::congroup_update_event evt;
	evt.set_type(draiosproto::REMOVED);
	auto cg = evt.mutable_object();
	cg->mutable_uid()->set_kind("container");
	cg->mutable_uid()->set_id(container_info.m_id);

	handle_event(&evt);
}

void infrastructure_state::receive_hosts_metadata(const google::protobuf::RepeatedPtrField<draiosproto::congroup_update_event> &host_events)
{
	m_host_events_queue_mutex.lock();
	glogf(sinsp_logger::SEV_DEBUG, "Lock and receive hosts metadata");
	for(auto hevt : host_events) {
		m_host_events_queue.emplace(std::move(hevt));
	}
	glogf(sinsp_logger::SEV_DEBUG, "%d hosts metadata received. Unlock.", m_host_events_queue.size());
	m_host_events_queue_mutex.unlock();
}

void infrastructure_state::clear_scope_cache()
{
	glogf(sinsp_logger::SEV_DEBUG, "Clear container/host scope cache because policies will be reloaded...");
	m_container_p_cache.clear();
	m_host_p_cache.clear();
}

void infrastructure_state::refresh_hosts_metadata()
{
	//
	// Remove current hosts
	//
	for (auto i = m_state.begin(); i != m_state.end();) {
		auto congroup = i->second.get();
		// remove all the links to host nodes
		if(host_children.find(congroup->uid().kind()) != host_children.end() || congroup->uid().kind() == "container") {
			for(auto j = congroup->mutable_parents()->begin(), j_end = congroup->mutable_parents()->end(); j != j_end; ++j) {
				if(j->kind() == "host") {
					congroup->mutable_parents()->erase(j);
					break;
				}
			}
		}

		if(congroup->uid().kind() == "host") {
			i = m_state.erase(i);
		} else {
			++i;
		}
	}

	//
	// Delete all cached results for host scope policies
	//
	m_host_p_cache.clear();

	glogf(sinsp_logger::SEV_INFO, "Adding %d hosts to infrastructure state", m_host_events_queue.size());

	//
	// Connect the refreshed data to the state
	//
	while(!m_host_events_queue.empty()) {

		auto& hevt = m_host_events_queue.front();
		auto host = hevt.mutable_object();

		glogf(sinsp_logger::SEV_DEBUG, "Add host %s to infrastructure state", host->uid().id().c_str());

		if (m_k8s_subscribed) {
			uid_t child_uid;
			bool has_child = true;
			std::vector<uid_t> nodes;

			for (auto i = m_state.begin(), e = m_state.end(); i != e; ++i) {
				auto congroup = i->second.get();
				if (host_children.find(congroup->uid().kind()) != host_children.end()) {
					bool found = false;
					for (auto j = congroup->ip_addresses().begin(), j_end = congroup->ip_addresses().end(); j != j_end; ++j) {
						for(auto k = host->ip_addresses().begin(), k_end = host->ip_addresses().end(); k != k_end; ++k) {
							if(*j == *k) {
								glogf(sinsp_logger::SEV_DEBUG, "Host %s match with congroup <%s,%s> for IP %s", host->uid().id().c_str(), congroup->uid().kind().c_str(), congroup->uid().id().c_str(), (*j).c_str());
								nodes.emplace_back(congroup->uid().kind(), congroup->uid().id());
								found = true;
								break;
							}
						}
						if (found) {
							break;
						}
					}
				}
			}

			if (nodes.empty()) {
				// this could also happen if the node has been removed but the backend didn't realized it yet
				glogf(sinsp_logger::SEV_WARNING, "Cannot match host %s, no suitable orchestrator nodes found.", host->uid().id().c_str());
				has_child = false;
			} else if(nodes.size() == 1) {
				child_uid = *nodes.begin();
			} else {
				glogf(sinsp_logger::SEV_WARNING, "Multiple matches while inserting metadata of host %s inside the infrastructure state", host->uid().id().c_str());

				//
				// Tiebreak based on hostName
				//
				bool found = false;
				if(host->tags().find("host.hostName") != host->tags().end()) {
					for(const auto c_uid : nodes) {
						const std::string& key = host_children.find(c_uid.first)->second;
						if(m_state[c_uid]->tags().find(key) != m_state[c_uid]->tags().end()) {
							std::string h_hn = m_state[c_uid]->tags().at(key);
							std::string n_hn = host->tags().at("host.hostName");
							std::transform(h_hn.begin(), h_hn.end(), h_hn.begin(), ::tolower);
							std::transform(n_hn.begin(), n_hn.end(), n_hn.begin(), ::tolower);
							if (h_hn == n_hn) {
								glogf(sinsp_logger::SEV_DEBUG, "hostName tiebreak found <%s,%s>", c_uid.first.c_str(), c_uid.second.c_str());
								found = true;
								child_uid = c_uid;
								break;
							}
						}
					}
				}

				if (!found) {
					glogf(sinsp_logger::SEV_WARNING, "Matching host %s when multiple agents matched based on IP but none matched on hostname", host->uid().id().c_str());
					child_uid = *nodes.begin();
				}
			}

			if(has_child) {
				//
				// Add the children link, handle_event will take care of connecting the host to the state
				//
				glogf(sinsp_logger::SEV_DEBUG, "Host %s is parent of <%s,%s>", host->uid().id().c_str(), child_uid.first.c_str(), child_uid.second.c_str());
				draiosproto::congroup_uid *c = host->mutable_children()->Add();
				c->set_kind(child_uid.first);
				c->set_id(child_uid.second);
			}
		}

		if(host->uid().id() == m_machine_id) {
			//
			// connect the local host to all the local containers
			//
			const auto containers_info = m_inspector->m_container_manager.get_containers();
			for(auto it = containers_info->begin(), it_end = containers_info->end(); it != it_end; ++it) {
				draiosproto::congroup_uid *c = host->mutable_children()->Add();
				c->set_kind("container");
				c->set_id(it->first);
			}
		}

		handle_event(&hevt);

		m_host_events_queue.pop();
	}
}

void infrastructure_state::debug_print()
{
	glogf(sinsp_logger::SEV_TRACE, "INFRASTRUCTURE STATE (size: %d)", m_state.size());

	for (auto it = m_state.begin(), e = m_state.end(); it != e; ++it) {
		draiosproto::container_group *cong = it->second.get();
		glogf(sinsp_logger::SEV_TRACE, " Container group <%s,%s>", cong->uid().kind().c_str(), cong->uid().id().c_str());
		glogf(sinsp_logger::SEV_TRACE, "  Tags:");
		for (auto t: cong->tags())
			glogf(sinsp_logger::SEV_TRACE, "   %s:%s", t.first.c_str(), t.second.c_str());
		glogf(sinsp_logger::SEV_TRACE, "  IP Addresses:");
		for (auto i: cong->ip_addresses())
			glogf(sinsp_logger::SEV_TRACE, "   %s", i.c_str());
		glogf(sinsp_logger::SEV_TRACE, "  Ports:");
		for (auto p: cong->ports())
			glogf(sinsp_logger::SEV_TRACE, "   %d:%s (target:%d, node:%d, published:%d)",
				  p.port(), p.protocol().c_str(), p.target_port(), p.node_port(), p.published_port());
		glogf(sinsp_logger::SEV_TRACE, "  Metrics:");
		for (auto m: cong->metrics())
			glogf(sinsp_logger::SEV_TRACE, "   %s:%g", m.name().c_str(), m.value());
		glogf(sinsp_logger::SEV_TRACE, "  Parents:");
		for (auto m: cong->parents())
			glogf(sinsp_logger::SEV_TRACE, "   <%s,%s>", m.kind().c_str(), m.id().c_str());
		glogf(sinsp_logger::SEV_TRACE, "  Children:");
		for (auto m: cong->children())
			glogf(sinsp_logger::SEV_TRACE, "   <%s,%s>", m.kind().c_str(), m.id().c_str());
	}
}

std::string infrastructure_state::get_k8s_cluster_name() const
{
	// XXX get the cluster name from GKE if possible
	return "default";
}

// The UID of the default namespace is used as the cluster id
std::string infrastructure_state::get_k8s_cluster_id() const
{
	if (!m_k8s_cached_cluster_id.empty()) {
		return m_k8s_cached_cluster_id;
	}

	// Skip ahead to namespaces then walk them sequentially
	uid_t lb_key("k8s_namespace", "");
	for (auto it = m_state.lower_bound(lb_key); it != m_state.end(); ++it) {
		// it.first is a uid_t
		// it.second is a container_group
		if (it->first.first != "k8s_namespace") {
			glogf(sinsp_logger::SEV_DEBUG,
			      "Unable to find default namespace for cluster id");
			break;
		}
		auto con_tags = it->second->tags();
		auto tag_iter = con_tags.find("kubernetes.namespace.name");
		// This "default" is the namespace name,
		// not to be confused with final return statement below
		if (tag_iter != con_tags.end() &&
		    tag_iter->second == "default") {
			m_k8s_cached_cluster_id = it->first.second;
			return m_k8s_cached_cluster_id;
		}
	}

	return "";
}
