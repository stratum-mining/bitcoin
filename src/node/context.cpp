// Copyright (c) 2019-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/context.h>

#include <addrman.h>
#include <banman.h>
#include <interfaces/chain.h>
#include <net.h>
#include <net_processing.h>
#include <policy/fees.h>
#include <scheduler.h>
#include <sv2_distributor.h>
#include <txmempool.h>
#include <validation.h>

NodeContext::NodeContext() {}
NodeContext::~NodeContext() {}
