#ifndef SV2_DISTRIBUTOR_H
#define SV2_DISTRIBUTOR_H

// TMP
#include <netbase.h>
#include <util/thread.h>
#include <util/syscall_sandbox.h>
#include <rusty/protocols/v2/sv2-ffi/sv2.h>
#include <chainparams.h>
#include <threadinterrupt.h>
// TMP

#include <streams.h>
#include <uint256.h>
#include <time.h>
#include <validation.h>
#include <miner.h>

class CNewTemplate;
class CCChainParams;

// TODO: Sv2Client that represents a remote downstream connection.
// TODO: Make this a private class in Sv2Distributor
class Sv2Client 
{
public:
    std::unique_ptr<Sock> m_sock;

    // TODO: TEMP to identify that a node has confirmed the SetupConnection Message
    // TODO: Should it also be atomic?
    bool m_setup_connection_confirmed;

    // TODO: Not sure if this unique_ptr as a param is correct and not sure if this type init constructor is acceptable in btc.
    Sv2Client(std::unique_ptr<Sock> sock) : m_sock{std::move(sock)}, m_setup_connection_confirmed{false} {};
};

class Sv2Distributor 
{
private:
    // TODO: Maybe not the right thing to do, maybe theres a reason for a vector
    // of sockets.
    // Also the naming convention for a signle member variable is probably wrong.
    // TODO: 
    std::unique_ptr<Sock> m_listening_socket;

    // TODO: 
    std::thread m_thread_sv2_handler;

    // TODO:
    std::vector<Sv2Client> m_sv2_clients;

    // TODO:
    std::atomic<bool> m_flag_interrupt_sv2{false};

    // TODO - Pass on construction?
    // - CChainstate*
    /* const CChainState& m_chainstate; */
    /* CChainState& m_chainstate; */
    ChainstateManager& m_chainman;

    // - CTXMempool*
    /* CTxMemPool& m_mempool; */
    CTxMemPool& m_mempool;

    // - CChainParams
    const CChainParams& m_chainparams;
    // - BlockAssembler options
    /* const BlockAssembler& m_block_assembler_options; */

    // TMP: Helper to see if we can throttle the wait on exiting IBD.
    CThreadInterrupt m_interrupt_sv2;

public:
    /* Sv2Distributor(CChainState& chainstate, CTxMemPool& mempool, const CChainParams& chainparams) */ 
        /* : m_chainstate{chainstate}, m_mempool{mempool}, m_chainparams{chainparams} {}; */
    Sv2Distributor(ChainstateManager& chainman, CTxMemPool& mempool, const CChainParams& chainparams) 
        : m_chainman{chainman}, m_mempool{mempool}, m_chainparams{chainparams} {};

    // TODO: Rename this to BuildSv2BlockTemplate
    CNewTemplate AssembleSv2BlockTemplate(const CBlock& block);
    void BindListenPort();
    void ThreadSv2Handler();
    void Start();
    void StopThreads();
    void Interrupt();
};
#endif // SV2_DISTRIBUTOR_H
