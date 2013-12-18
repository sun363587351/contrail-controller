/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_kstate_h
#define vnsw_agent_kstate_h

#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include <base/logging.h>
#include <db/db_entry.h>
#include <db/db_table.h>
#include <db/db_table_partition.h>

#include <ksync/ksync_index.h>
#include <ksync/ksync_entry.h>
#include <ksync/ksync_object.h>
#include <ksync/ksync_sock.h>

#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include "vr_types.h"
#include "kstate/kstate_types.h"
#include "nl_util.h"


class KState : public AgentSandeshContext {
public:
    static const int max_entries_per_response = 100;
    KState(std::string s, Sandesh *obj) : resp_ctx_(s), 
        resp_obj_(obj), k_resp_code_(0), more_ctx_(NULL) {}

    void EncodeAndSend(Sandesh &encoder);
    virtual void SendResponse() = 0;
    virtual void SendNextRequest() = 0;
    virtual void Handler() = 0;
    void UpdateContext(void *);
    /* The following API is invoked from agent main to make sure that this library is 
     * linked with agent */
    static void Init();
    virtual void Release() {
        if (resp_obj_) {
            resp_obj_->Release();
            resp_obj_ = NULL;
        }
    }
    std::string resp_ctx() { return resp_ctx_; }
    Sandesh *resp_obj() { return resp_obj_; }
    void *more_ctx() { return more_ctx_; }
    void set_k_resp_code(int value) { k_resp_code_ = value; }
    bool MoreData();
    virtual void IfMsgHandler(vr_interface_req *req);
    virtual void NHMsgHandler(vr_nexthop_req *req);
    virtual void RouteMsgHandler(vr_route_req *req);
    virtual void MplsMsgHandler(vr_mpls_req *req);
    virtual void MirrorMsgHandler(vr_mirror_req *req);
    virtual int VrResponseMsgHandler(vr_response *r);
    virtual void FlowMsgHandler(vr_flow_req *req) {}
    virtual void VrfAssignMsgHandler(vr_vrf_assign_req *req);
    virtual void VrfStatsMsgHandler(vr_vrf_stats_req *req);
    virtual void DropStatsMsgHandler(vr_drop_stats_req *req);
    virtual void VxLanMsgHandler(vr_vxlan_req *req);
protected:
    std::string resp_ctx_;
    Sandesh *resp_obj_;
    int k_resp_code_; /* response code from kernel */
    void *more_ctx_; /* context to hold marker info */
};

#endif // vnsw_agent_kstate_h
