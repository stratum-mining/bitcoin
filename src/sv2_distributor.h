#ifndef SV2_DISTRIBUTOR_H
#define SV2_DISTRIBUTOR_H

#include <streams.h>
#include <uint256.h>
#include <time.h>
#include <validation.h>
#include <miner.h>
#include <rusty/protocols/v2/sv2-ffi/sv2.h>

CNewTemplate AssembleSv2BlockTemplate(CChainState& chainstate, const CTxMemPool& mempool, const CChainParams& params, const BlockAssembler::Options options) {
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(chainstate, mempool, params, options)
        .CreateNewBlock(CScript());

    const CBlock block = pblocktemplate->block;
    const CBlockHeader header = block.GetBlockHeader();
    
    CNewTemplate ctemplate;
    ctemplate.template_id = GetTimeSeconds();

    // TODO: Decide when this is a future block or not.
    ctemplate.future_template = false;
    ctemplate.version = header.nVersion;

    const auto coinbase_tx = block.vtx[0];
    ctemplate.coinbase_tx_version = coinbase_tx->CURRENT_VERSION;

    CDataStream coinbase_script(SER_NETWORK, PROTOCOL_VERSION);
    coinbase_script << coinbase_tx->vin[0].scriptSig;

    // TODO: Double check why 8 is hardcoded as the length?
    ctemplate.coinbase_prefix = cvec_from_buffer(&coinbase_script[0], 8);
    ctemplate.coinbase_tx_input_sequence = coinbase_tx->vin[0].nSequence;

    // TODO: Can keep this set to 0, since this will be modified by the client?
    ctemplate.coinbase_tx_value_remaining = 0;
    ctemplate.coinbase_tx_outputs_count = coinbase_tx->vout.size();
    
    CDataStream vout(SER_NETWORK, PROTOCOL_VERSION);
    vout << coinbase_tx->vout;
    ctemplate.coinbase_tx_outputs = cvec_from_buffer(&vout[0], vout.size());
    ctemplate.coinbase_tx_locktime = coinbase_tx->nLockTime;

    CVec2 cvec2 = init_cvec2();
    for (const auto& tx: block.vtx) {
        CDataStream merkle_path_stream(SER_NETWORK, PROTOCOL_VERSION);
        merkle_path_stream << tx->GetHash();

        auto merkle_path = cvec_from_buffer(&merkle_path_stream[0], merkle_path_stream.size());
        cvec2_push(&cvec2, merkle_path);
    }

    ctemplate.merkle_path = cvec2;

    return ctemplate;
}
#endif // SV2_DISTRIBUTOR_H
