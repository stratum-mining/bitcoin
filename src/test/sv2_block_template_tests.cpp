#include <chainparams.h>
#include <streams.h>
#include <uint256.h>
#include <miner.h>
#include <rusty/protocols/v2/sv2-ffi/sv2.h>
#include <time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>


// TODO: This is just copied from miner_tests.cpp
// TODO: 
// - [] What is the purpose of this namespace?
// - [] What is TestingSetup?
namespace sv2_block_template_tests {
struct Sv2BlockTemplateTestingSetup : public TestingSetup {

    void TestPackageSelection(
		    const CChainParams& chainparams, 
		    const CScript& scriptPubKey, 
		    const std::vector<CTransactionRef>& txFirst) 
	    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, m_node.mempool->cs);

    bool TestSequenceLocks(const CTransaction& tx, int flags) 
	    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, m_node.mempool->cs)
    {
        CCoinsViewMemPool view_mempool(&m_node.chainman->ActiveChainstate().CoinsTip(), *m_node.mempool);
        return CheckSequenceLocks(m_node.chainman->ActiveChain().Tip(), view_mempool, tx, flags);
    }

    BlockAssembler AssemblerForTest(const CChainParams& params);
};
} // namespace sv2_block_template_tests

// TODO: 
// - [] What is BOOST_FIXTURE_TEST_SUITE
BOOST_FIXTURE_TEST_SUITE(sv2_block_template_tests, Sv2BlockTemplateTestingSetup)

// TODO: This is just copied from miner_tests.cpp
static CFeeRate blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
// TODO: This is just copied from miner_tests.cpp
BlockAssembler Sv2BlockTemplateTestingSetup::AssemblerForTest(const CChainParams& params)
{
    BlockAssembler::Options options;

    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT;
    options.blockMinFeeRate = blockMinFeeRate;
    return BlockAssembler(m_node.chainman->ActiveChainstate(), *m_node.mempool, params, options);
}

BOOST_AUTO_TEST_CASE(create_sv2_block_template)
{
    // TODO:
    // 1. Use the BlockAssembler to create a block?
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const CChainParams& chainparams = *chainParams;
    auto block_template = AssemblerForTest(chainparams).CreateNewBlock(CScript());

    // TODO:
    // Extract this into a class or function that accepts a block and extracts
    // what is required.
    const CBlockHeader header = block_template->block.GetBlockHeader();
    CNewTemplate ctemplate;

    // TODO: Replace this with the unix timestamp.
    ctemplate.template_id = GetTimeSeconds();
    ctemplate.future_template = false;
    ctemplate.version = header.nVersion;

    const auto coinbase_tx = block_template->block.vtx[0];
    ctemplate.coinbase_tx_version = coinbase_tx->CURRENT_VERSION;

    // TODO:
    // I have no idea if any of this is correct.
    CDataStream coinbase_script(SER_NETWORK, PROTOCOL_VERSION);
    coinbase_script << coinbase_tx->vin[0].scriptSig;
    ctemplate.coinbase_prefix = cvec_from_buffer(&coinbase_script[0], 8);
    ctemplate.coinbase_tx_input_sequence = coinbase_tx->vin[0].nSequence;

    // TODO: Can keep this set to 0, since this will be modified by the client?
    ctemplate.coinbase_tx_value_remaining = 0;

    ctemplate.coinbase_tx_outputs_count = coinbase_tx->vout.size();
    
    // TODO: I don't know if the is the best way to deserialize objects to bytes
    // and then assign to a cvec buffer?
    // TODO: 
    // - [] Should I be using CDataStream?
    // - [] What does SER_NETWORK
    CDataStream vout(SER_NETWORK, PROTOCOL_VERSION);
    vout << coinbase_tx->vout;
    ctemplate.coinbase_tx_outputs = cvec_from_buffer(&vout[0], vout.size());
    ctemplate.coinbase_tx_locktime = coinbase_tx->nLockTime;

    // TODO:
    // I have no idea if any of this is correct.
    CDataStream merkle_path_stream(SER_NETWORK, PROTOCOL_VERSION);
    for (const auto& tx: block_template->block.vtx) {
        merkle_path_stream << tx->GetHash();
    }
    auto cvec_merkle_path = cvec_from_buffer(&merkle_path_stream[0], merkle_path_stream.size());
    ctemplate.merkle_path = CVec2{&cvec_merkle_path, merkle_path_stream.size(), merkle_path_stream.size()};
}

BOOST_AUTO_TEST_SUITE_END()
