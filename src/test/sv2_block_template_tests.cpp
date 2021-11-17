#include <chainparams.h>
#include <streams.h>
#include <uint256.h>
#include <miner.h>

#include <sv2_distributor.h>
#include <time.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>


// CCDLE12: TMP - Remove once mock mempool can be used.
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
};
} // namespace sv2_block_template_tests

BOOST_FIXTURE_TEST_SUITE(sv2_block_template_tests, Sv2BlockTemplateTestingSetup)

BOOST_AUTO_TEST_CASE(create_sv2_block_template)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const CChainParams& chainparams = *chainParams;

    BlockAssembler::Options options;
    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT;
    options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

    CNewTemplate ctemplate = AssembleSv2BlockTemplate(m_node.chainman->ActiveChainstate(), *m_node.mempool, chainparams, options);

    CSv2Message message;
    message.tag = CSv2Message::Tag::NewTemplate;
    message.new_template._0 = ctemplate;

    EncoderWrapper* encoder = new_encoder();
    CResult<CVec, Sv2Error> encoded = encode(&message, encoder);

    // CCDLE12: TMP Debug
    switch (encoded.tag) {
    case CResult < CVec, Sv2Error > ::Tag::Ok:
        std::cout << "Encoding was OK" << std::endl;
        break;
    case CResult < CVec, Sv2Error > ::Tag::Err:
        std::cout << "ERROR OCCURRED: " << encoded.err._0 << std::endl;
        break;
    }

    DecoderWrapper* decoder = new_decoder();

    std::cout << "CVEC ENCODED BUFFER LEN: " << encoded.ok._0.len << std::endl;
    std::cout << "CVEC ENCODED BUFFER Capacity: " << encoded.ok._0.capacity << std::endl;


    int byte_read = 0;
    bool decoded = false;
    while (! decoded) {
        // This is thought to works with streams of data from which you read new bytes when they
        // arrive so more something like that:
        // ```
        // CVec buffer = get_writable(decoder);
        // while (byte_read < buffer.len) {
        //   byte_read += read(new_socket, buffer.data, (buffer.len - byte_read));

        // }
        // byte_read = 0;
        // ```

        CVec buffer = get_writable(decoder);
        memcpy(buffer.data, &encoded.ok._0.data[byte_read], buffer.len);
        byte_read += buffer.len;

        CResult < CSv2Message, Sv2Error > frame = next_frame(decoder);

        switch (frame.tag) {
          case CResult < CSv2Message, Sv2Error > ::Tag::Ok:
            std::cout << "\n";
            std::cout << "OK";
            std::cout << "\n";
            decoded = true;
            break;
          case CResult < CSv2Message, Sv2Error > ::Tag::Err:
            break;
          };
    }


    // TMP: NOTES
    // 1. Size Hint in framing2.rs should return 0.
    //
    // 2. Hint is calculated using:
    //   - bytes.len() - Header::SIZE == header.len()
    //
    // 3. Variable Sizes:
    //   - Header::SIZE == 6
    //   - bytes == 6
    //   - header.len (msg.len) == 138
    //
    // 4. 6 - 6 == 0 (should equal 138) but is 0
    //
    // 5. bytes should equal 144
    //   - So it's not actually finding the rest of the 138 bytes
    //memcpy(buffer.data, encoded.ok._0.data, encoded.ok._0.len);
    //std::cout << "DEBUG: Bytes at buffer and encoded are equal: " << memcmp(buffer.data, encoded.ok._0.data, encoded.ok._0.len) << std::endl;

    //CResult < CSv2Message, Sv2Error > frame = next_frame(decoder);
    // switch (frame.tag) {
    // case CResult < CSv2Message, Sv2Error > ::Tag::Ok:
    //     std::cout << "DECODING was OK" << std::endl;
    //     break;
    // case CResult < CSv2Message, Sv2Error > ::Tag::Err:
    //     std::cout << "DECODING ERROR OCCURRED: " << frame.err._0 << std::endl;
    // switch (frame.err._0) {
	  //   case Sv2Error::MissingBytes:
	  // 	  std::cout << "Waiting for the remaining part of the frame \n";
	  //     break;
	  //   case Sv2Error::Unknown:
	  //     std::cout << "An unknown error occured \n";
	  //     break;
	  //   }
    //       break;
	  // }
}

BOOST_AUTO_TEST_SUITE_END()
