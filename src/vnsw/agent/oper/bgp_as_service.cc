/*
 * Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
 */

#include <string>
#include <vector>
#include <vnc_cfg_types.h>
#include <base/logging.h>
#include <base/string_util.h>
#include <base/bgp_as_service_utils.h>
#include <boost/uuid/uuid_io.hpp>
#include <cmn/agent_cmn.h>
#include <init/agent_param.h>
#include <ifmap/ifmap_node.h>
#include <ifmap/ifmap_link.h>
#include <cfg/cfg_init.h>
#include <cmn/agent.h>
#include <oper/operdb_init.h>
#include <oper/bgp_as_service.h>
#include <oper/audit_list.h>
#include <oper/agent_sandesh.h>
#include <oper/config_manager.h>
#include <oper/vn.h>
#include <bgp_schema_types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include "net/address_util.h"
#include "oper/global_system_config.h"
#include <resource_manager/resource_manager.h>
#include <resource_manager/resource_table.h>
#include <resource_manager/bgp_as_service_index.h>

using namespace std;
SandeshTraceBufferPtr BgpAsAServiceTraceBuf(SandeshTraceBufferCreate
                                            ("BgpAsAService", 500));

BgpAsAService::BgpAsAService(const Agent *agent) :
    agent_(agent),
    bgp_as_a_service_entry_map_(),
    bgp_as_a_service_port_map_(),
    service_delete_cb_(), health_check_cb_() {
}

BgpAsAService::~BgpAsAService() {
}

void BgpAsAService::BindBgpAsAServicePorts(const std::vector<uint16_t> &ports) {
    if (ports.size() != 2) {
        BGPASASERVICETRACE(Trace, "Port bind range rejected - parsing failed");
        return;
    }

    uint16_t start = ports[0];
    uint16_t end = ports[1];

    for (uint16_t port = start; port <= end; port++) {
        int port_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in address;
        memset(&address, '0', sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(agent_->router_id().to_ulong());
        address.sin_port = htons(port);
        int optval = 1;
        if (fcntl(port_fd, F_SETFD, FD_CLOEXEC) < 0) {
            std::stringstream ss;
            ss << "Port setting fcntl failed with error ";
            ss << strerror(errno);
            ss << " for port ";
            ss << port;
            BGPASASERVICETRACE(Trace, ss.str().c_str());
        }
        setsockopt(port_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval));
        if (bind(port_fd, (struct sockaddr*) &address,
                 sizeof(sockaddr_in)) < 0) {
            std::stringstream ss;
            ss << "Port bind failed for port# ";
            ss << port;
            ss << " with error ";
            ss << strerror(errno);
            BGPASASERVICETRACE(Trace, ss.str().c_str());
        }
    }
}

const BgpAsAService::BgpAsAServiceEntryMap &BgpAsAService::bgp_as_a_service_map() const {
    return bgp_as_a_service_entry_map_;
}

const BgpAsAService::BgpAsAServicePortMap &BgpAsAService::bgp_as_a_service_port_map() const {
    return bgp_as_a_service_port_map_;
}

void BgpAsAService::BgpAsAServiceList::Insert(const BgpAsAServiceEntry *rhs) {
    if (rhs->health_check_configured_)
        rhs->new_health_check_add_ = true;
    list_.insert(*rhs);
}

void BgpAsAService::BgpAsAServiceList::Update(const BgpAsAServiceEntry *lhs,
                                              const BgpAsAServiceEntry *rhs) {
    if (lhs->health_check_configured_ != rhs->health_check_configured_) {
        lhs->health_check_configured_ = rhs->health_check_configured_;
        if (lhs->health_check_configured_) {
            lhs->new_health_check_add_ = true;
            lhs->health_check_uuid_ = rhs->health_check_uuid_;
        } else {
            lhs->old_health_check_delete_ = true;
            lhs->old_health_check_uuid_ = lhs->health_check_uuid_;
        }
    }
    if (lhs->health_check_uuid_ != rhs->health_check_uuid_) {
        // delete old health check and add new
        lhs->new_health_check_add_ = true;
        lhs->old_health_check_delete_ = true;
        lhs->old_health_check_uuid_ = lhs->health_check_uuid_;
        lhs->health_check_uuid_ = rhs->health_check_uuid_;
    }
}

void BgpAsAService::BgpAsAServiceList::Remove(BgpAsAServiceEntryListIterator &it) {
    it->set_del_pending(true);
}

void BgpAsAService::BgpAsAServiceList::Flush() {
    list_.clear();
}

static const std::string GetBgpRouterVrfName(const Agent *agent,
                                             IFMapNode *node) {
    IFMapAgentTable *table = static_cast<IFMapAgentTable *>(node->table());
    for (DBGraphVertex::adjacency_iterator it = node->begin(table->GetGraph());
         it != node->end(table->GetGraph()); ++it) {
        IFMapNode *vrf_node = static_cast<IFMapNode *>(it.operator->());
        if (agent->config_manager()->SkipNode
            (vrf_node, agent->cfg()->cfg_vrf_table())) {
            continue;
        }
        return vrf_node->name();
    }
    return std::string();
}

void BgpAsAService::BuildBgpAsAServiceInfo(IFMapNode *bgp_as_a_service_node,
                                           BgpAsAServiceEntryList &new_list,
                                           const std::string &vm_vrf_name,
                                           const boost::uuids::uuid &vm_uuid) {
    IFMapAgentTable *table =
        static_cast<IFMapAgentTable *>(bgp_as_a_service_node->table());
    autogen::BgpAsAService *bgp_as_a_service =
        dynamic_cast<autogen::BgpAsAService *>(bgp_as_a_service_node->GetObject());
    assert(bgp_as_a_service);

    IpAddress peer_ip;
    uint32_t source_port = 0;
    bool health_check_configured = false;
    boost::uuids::uuid health_check_uuid;

    // Find the health check config first
    for (DBGraphVertex::adjacency_iterator it = bgp_as_a_service_node->begin(table->GetGraph());
         it != bgp_as_a_service_node->end(table->GetGraph()); ++it) {
        IFMapNode *adj_node = static_cast<IFMapNode *>(it.operator->());

        if (adj_node->table() == agent_->cfg()->cfg_health_check_table()) {
            if (agent_->config_manager()->SkipNode(adj_node)) {
                continue;
            }
            autogen::ServiceHealthCheck *hc =
                static_cast<autogen::ServiceHealthCheck *> (adj_node->GetObject());
            assert(hc);
            // consider only BFD health check for BGPaaS
            if (hc->properties().monitor_type.find("BFD") == std::string::npos)
                continue;
            health_check_configured = true;
            autogen::IdPermsType id_perms = hc->id_perms();
            CfgUuidSet(id_perms.uuid.uuid_mslong, id_perms.uuid.uuid_lslong,
                       health_check_uuid);
        }
    }

    //Look for neighbour bgp-router to take the source port
    for (DBGraphVertex::adjacency_iterator it = bgp_as_a_service_node->begin(table->GetGraph());
         it != bgp_as_a_service_node->end(table->GetGraph()); ++it) {
        IFMapNode *adj_node = static_cast<IFMapNode *>(it.operator->());
        if (agent_->config_manager()->SkipNode(adj_node)) {
            continue;
        }
        if (strcmp(adj_node->table()->Typename(), BGP_ROUTER_CONFIG_NAME) == 0) {
            autogen::BgpRouter *bgp_router=
                dynamic_cast<autogen::BgpRouter *>(adj_node->GetObject());
            const std::string &vrf_name =
                GetBgpRouterVrfName(agent_, adj_node);
            if (vrf_name.empty() || (vrf_name != vm_vrf_name) ||
                (strcmp(bgp_router->parameters().router_type.c_str(),
                        VALID_BGP_ROUTER_TYPE) != 0))
                continue; //Skip the node with no VRF, notification will come.
            boost::system::error_code ec;
            peer_ip =
                IpAddress::from_string(bgp_router->parameters().address, ec);
            if (ec.value() != 0) {
                std::stringstream ss;
                ss << "Ip address parsing failed for ";
                ss << bgp_router->parameters().address;
                BGPASASERVICETRACE(Trace, ss.str().c_str());
                continue;
            }

            BgpAsAServiceEntryMapIterator old_bgp_as_a_service_entry_list_iter =
                                    bgp_as_a_service_entry_map_.find(vm_uuid);
            source_port = 0;
            /*
             *  verify the same session is added again here
             */
            if (old_bgp_as_a_service_entry_list_iter !=
                        bgp_as_a_service_entry_map_.end()) {
                BgpAsAServiceEntryList temp_bgp_as_a_service_entry_list;
                temp_bgp_as_a_service_entry_list.insert(
                                    BgpAsAServiceEntry(peer_ip,
                                        bgp_router->parameters().source_port,
                                        health_check_configured,
                                        health_check_uuid));
                /*
                 * if it is same session then retain original source port
                 */
                if (temp_bgp_as_a_service_entry_list == 
                     old_bgp_as_a_service_entry_list_iter->second->list_) {
                    source_port = bgp_router->parameters().source_port;
                }
            }
            if (!source_port) {
                source_port = AddBgpVmiServicePortIndex(
                                    bgp_router->parameters().source_port,
                                    vm_uuid);
            }
            if (source_port) {
                new_list.insert(BgpAsAServiceEntry(peer_ip, source_port,
                                                   health_check_configured,
                                                   health_check_uuid));
            }
        }
    }

}

void BgpAsAService::ProcessConfig(const std::string &vrf_name,
                                  std::list<IFMapNode *> &node_map,
                                  const boost::uuids::uuid &vm_uuid) {
    std::list<IFMapNode *>::const_iterator it =
        node_map.begin();
    BgpAsAServiceEntryList new_bgp_as_a_service_entry_list;

    while (it != node_map.end()) {
        BuildBgpAsAServiceInfo(*it, new_bgp_as_a_service_entry_list,
                               vrf_name, vm_uuid);
        it++;
    }

    //Audit and enqueue updates/deletes of flow
    BgpAsAServiceEntryMapIterator old_bgp_as_a_service_entry_list_iter =
        bgp_as_a_service_entry_map_.find(vm_uuid);
    bool changed = false;
    if (old_bgp_as_a_service_entry_list_iter !=
        bgp_as_a_service_entry_map_.end()) {
        //Audit
        changed = AuditList<BgpAsAServiceList, BgpAsAServiceEntryListIterator>
            (*(old_bgp_as_a_service_entry_list_iter->second),
             old_bgp_as_a_service_entry_list_iter->second->list_.begin(),
             old_bgp_as_a_service_entry_list_iter->second->list_.end(),
             new_bgp_as_a_service_entry_list.begin(),
             new_bgp_as_a_service_entry_list.end());
    } else if (new_bgp_as_a_service_entry_list.size() != 0) {
        bgp_as_a_service_entry_map_[vm_uuid] =
            new BgpAsAServiceList(new_bgp_as_a_service_entry_list);
    }

    if (changed && !service_delete_cb_.empty()) {
        //Enqueue flow handler request.
        BgpAsAServiceEntryListIterator iter =
            old_bgp_as_a_service_entry_list_iter->second->list_.begin();
        while (iter !=
               old_bgp_as_a_service_entry_list_iter->second->list_.end()) {
            BgpAsAServiceEntryListIterator prev = iter++;
            if (prev->del_pending_) {
                service_delete_cb_(vm_uuid, prev->source_port_);
                FreeBgpVmiServicePortIndex(prev->source_port_);
                old_bgp_as_a_service_entry_list_iter->second->list_.erase(prev);
                continue;
            }
            if (prev->old_health_check_delete_) {
                if (!health_check_cb_.empty() &&
                    prev->old_health_check_uuid_ != nil_uuid()) {
                    health_check_cb_(vm_uuid, prev->source_port_,
                                     prev->old_health_check_uuid_, false);
                }
                prev->old_health_check_delete_ = false;
                prev->old_health_check_uuid_ = nil_uuid();
            }
            if (prev->new_health_check_add_) {
                if (!health_check_cb_.empty() &&
                    prev->health_check_uuid_ != nil_uuid()) {
                    health_check_cb_(vm_uuid, prev->source_port_,
                                     prev->health_check_uuid_, true);
                }
                prev->new_health_check_add_ = false;
            }
        }
    }
}

void BgpAsAService::DeleteVmInterface(const boost::uuids::uuid &vm_uuid) {
    if (service_delete_cb_.empty())
        return;

    BgpAsAServiceEntryMapIterator iter =
       bgp_as_a_service_entry_map_.find(vm_uuid);
    if (iter == bgp_as_a_service_entry_map_.end())
        return;

    BgpAsAServiceEntryList list = iter->second->list_;
    BgpAsAServiceEntryListIterator list_iter = list.begin();
    while (list_iter != list.end()) {
        service_delete_cb_(vm_uuid, (*list_iter).source_port_);
        FreeBgpVmiServicePortIndex((*list_iter).source_port_);
        list_iter++;
    }
    delete iter->second;
    bgp_as_a_service_entry_map_.erase(iter);
}


bool BgpAsAService::IsBgpService(const VmInterface *vm_intf,
                                 const IpAddress &source_ip,
                                 const IpAddress &dest_ip) const {
    bool ret = false;
    BgpAsAServiceEntryMapConstIterator iter =
       bgp_as_a_service_entry_map_.find(vm_intf->GetUuid());
    if (iter == bgp_as_a_service_entry_map_.end()) {
        return false;
    }

    while (iter != bgp_as_a_service_entry_map_.end()) {
        BgpAsAService::BgpAsAServiceEntryListIterator it =
            iter->second->list_.begin();
        while (it != iter->second->list_.end()) {
            if ((*it).local_peer_ip_ == source_ip)
                return true;
            it++;
        }
        iter++;
    }

    const VnEntry *vn = vm_intf->vn();
    if (vn == NULL) return false;

    const IpAddress &vm_ip = vm_intf->primary_ip_addr();
    if ((vn->GetGatewayFromIpam(vm_ip) == dest_ip) ||
        (vn->GetDnsFromIpam(vm_ip) == dest_ip)) {
        ret = true;
    }
    return ret;
}

void BgpAsAService::FreeBgpVmiServicePortIndex(const uint32_t sport) {
    BGPaaServiceParameters:: BGPaaServicePortRangePair ports =
                agent_->oper_db()->global_system_config()->bgpaas_port_range();
    BGPaaSUtils::BgpAsServicePortIndexPair portinfo =
                    BGPaaSUtils::DecodeBgpaasServicePort(sport,
                        ports.first, ports.second);

    BgpAsAServicePortMapIterator port_map_it =
                    bgp_as_a_service_port_map_.find(portinfo.first);
    if (port_map_it == bgp_as_a_service_port_map_.end()) {
        return;
    }

    size_t vmi_service_port_index = portinfo.second;
    agent_->resource_manager()->Release(Resource::BGP_AS_SERVICE_INDEX,
                                        vmi_service_port_index);
    port_map_it->second->Remove(vmi_service_port_index);

    if (port_map_it->second->NoneIndexSet()) {
        delete port_map_it->second;
        bgp_as_a_service_port_map_.erase(port_map_it);
    }
}

size_t BgpAsAService::AllocateBgpVmiServicePortIndex(const uint32_t sport,
                                        const boost::uuids::uuid vm_uuid) {
    BgpAsAServicePortMapIterator port_map_it =
                    bgp_as_a_service_port_map_.find(sport);
    if (port_map_it == bgp_as_a_service_port_map_.end()) {
        bgp_as_a_service_port_map_[sport] = new IndexVector<boost::uuids::uuid>();
    }
    ResourceManager::KeyPtr rkey(new BgpAsServiceIndexResourceKey
                                 (agent_->resource_manager(), vm_uuid));
    uint32_t index = static_cast<IndexResourceData *>
        (agent_->resource_manager()->Allocate(rkey).get())->index();
    bgp_as_a_service_port_map_[sport]->InsertAtIndex(index, vm_uuid);
    return index;
}

uint32_t BgpAsAService::AddBgpVmiServicePortIndex(const uint32_t source_port,
                                            const boost::uuids::uuid vm_uuid) {
    BGPaaServiceParameters:: BGPaaServicePortRangePair ports =
                agent_->oper_db()->global_system_config()->bgpaas_port_range();

    size_t vmi_service_port_index = AllocateBgpVmiServicePortIndex(source_port,
                                                                   vm_uuid);
    if (vmi_service_port_index == BitSet::npos) {
        std::stringstream ss;
        ss << "Service Port Index is not available for ";
        ss << source_port;
        BGPASASERVICETRACE(Trace, ss.str().c_str());
        return 0;
    }
    return BGPaaSUtils::EncodeBgpaasServicePort(
                                source_port,
                                vmi_service_port_index,
                                ports.first, ports.second);
}

bool BgpAsAService::GetBgpRouterServiceDestination(
                    const VmInterface *vm_intf, const IpAddress &source_ip,
                    const IpAddress &dest, IpAddress *nat_server,
                    uint32_t *sport, bool *health_check_configured,
                    boost::uuids::uuid *health_check_uuid) const {
    const VnEntry *vn = vm_intf->vn();
    if (vn == NULL) return false;

    const IpAddress &vm_ip = vm_intf->primary_ip_addr();
    const IpAddress &gw = vn->GetGatewayFromIpam(vm_ip);
    const IpAddress &dns = vn->GetDnsFromIpam(vm_ip);

    boost::system::error_code ec;
    BgpAsAServiceEntryMapConstIterator map_it =
        bgp_as_a_service_entry_map_.find(vm_intf->GetUuid());
    if (map_it == bgp_as_a_service_entry_map_.end()) return false;

    BgpAsAServiceEntryListConstIterator it = map_it->second->list_.begin();
    while (it != map_it->second->list_.end()) {
        *health_check_configured = it->health_check_configured_;
        *health_check_uuid = it->health_check_uuid_;
        if (dest == gw) {
            if (agent_->controller_ifmap_xmpp_server(0).empty())
                return false;
            *nat_server =
                IpAddress::from_string(agent_->
                                       controller_ifmap_xmpp_server(0), ec);
            if (ec.value() != 0) {
                std::stringstream ss;
                ss << "Ip address parsing failed for ";
                ss << agent_->controller_ifmap_xmpp_server(0);
                BGPASASERVICETRACE(Trace, ss.str().c_str());
                return false;
            }
            *sport = it->source_port_;
            return true;
        }
        if (dest == dns) {
            if (agent_->controller_ifmap_xmpp_server(1).empty())
                return false;
            *nat_server =
                IpAddress::from_string(agent_->
                                       controller_ifmap_xmpp_server(1), ec);
            if (ec.value() != 0) {
                std::stringstream ss;
                ss << "Ip address parsing failed for ";
                ss << agent_->controller_ifmap_xmpp_server(1);
                BGPASASERVICETRACE(Trace, ss.str().c_str());
                return false;
            }
            *sport = it->source_port_;
            return true;
        }
        it++;
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////
// BGP as a service routines.
////////////////////////////////////////////////////////////////////////////
BgpAsAService::BgpAsAServiceEntry::BgpAsAServiceEntry() :
    VmInterface::ListEntry(), installed_(false),
    local_peer_ip_(), source_port_(0), health_check_configured_(false),
    health_check_uuid_(), new_health_check_add_(false),
    old_health_check_delete_(false), old_health_check_uuid_() {
}

BgpAsAService::BgpAsAServiceEntry::BgpAsAServiceEntry
(const BgpAsAService::BgpAsAServiceEntry &rhs) :
    VmInterface::ListEntry(rhs.del_pending_),
    installed_(rhs.installed_),
    local_peer_ip_(rhs.local_peer_ip_), 
    source_port_(rhs.source_port_),
    health_check_configured_(rhs.health_check_configured_),
    health_check_uuid_(rhs.health_check_uuid_),
    new_health_check_add_(rhs.new_health_check_add_),
    old_health_check_delete_(rhs.old_health_check_delete_),
    old_health_check_uuid_(rhs.old_health_check_uuid_) {
}

BgpAsAService::BgpAsAServiceEntry::BgpAsAServiceEntry
(const IpAddress &local_peer_ip, uint32_t source_port,
 bool health_check_configured, const boost::uuids::uuid &health_check_uuid) :
    VmInterface::ListEntry(),
    installed_(false),
    local_peer_ip_(local_peer_ip),
    source_port_(source_port),
    health_check_configured_(health_check_configured),
    health_check_uuid_(health_check_uuid), new_health_check_add_(false),
    old_health_check_delete_(false), old_health_check_uuid_() {
}

BgpAsAService::BgpAsAServiceEntry::~BgpAsAServiceEntry() {
}

bool BgpAsAService::BgpAsAServiceEntry::operator ==
    (const BgpAsAServiceEntry &rhs) const {
    return ((source_port_ == rhs.source_port_) &&
            (local_peer_ip_ == rhs.local_peer_ip_));
}

bool BgpAsAService::BgpAsAServiceEntry::operator()
    (const BgpAsAServiceEntry &lhs, const BgpAsAServiceEntry &rhs) const {
    return lhs.IsLess(&rhs);
}

bool BgpAsAService::BgpAsAServiceEntry::IsLess
    (const BgpAsAServiceEntry *rhs) const {
    if (source_port_ != rhs->source_port_)
        return source_port_ < rhs->source_port_;
    return local_peer_ip_ < rhs->local_peer_ip_;
}

void BgpAsAServiceSandeshReq::HandleRequest() const {
   BgpAsAServiceSandeshResp *resp = new BgpAsAServiceSandeshResp();
   resp->set_context(context());

   Agent *agent = Agent::GetInstance();

   BgpAsAService::BgpAsAServiceEntryMap map_entry =
       agent->oper_db()->bgp_as_a_service()->bgp_as_a_service_map();
   BgpAsAService::BgpAsAServiceEntryMapIterator map_it =
       map_entry.begin();
   std::vector<BgpAsAServiceSandeshList> bgpaas_map;
   while (map_it != map_entry.end()) {
       BgpAsAService::BgpAsAServiceEntryListIterator it =
           map_it->second->list_.begin();
       while (it != map_it->second->list_.end()) {
           BgpAsAServiceSandeshList entry;
           entry.set_vm_bgp_peer_ip((*it).local_peer_ip_.to_string());
           entry.set_vm_nat_source_port((*it).source_port_);
           entry.set_vmi_uuid(UuidToString(map_it->first));
           entry.set_health_check_configured((*it).health_check_configured_);
           entry.set_health_check_uuid(UuidToString((*it).health_check_uuid_));
           bgpaas_map.push_back(entry);
           it++;
       }
       map_it++;
   }
   resp->set_bgp_as_a_service_list(bgpaas_map);
   resp->Response();
}
