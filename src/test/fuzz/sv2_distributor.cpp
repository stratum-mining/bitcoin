#include <sv2_distributor.h>

#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>


FUZZ_TARGET(sv2_distributor)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};

    // TODO:
    // 0. [x] Run the fuzz test without anything
    // 1. Create a sv2 decoder
    std::vector<uint8_t> data = ConsumeRandomLengthByteVector(fuzzed_data_provider);
    DecoderWrapper* decoder = new_decoder();
    CVec cvec_buffer = get_writable(decoder);

    // 2. Add the input fuzz data
    int byte_read = 0;
    bool decoded = false;
    CNewTemplate decoded_msg;
    while (!decoded) {
        // TODO: Why does replacing a cvec_buffer make this work?
        cvec_buffer = get_writable(decoder);
        memcpy(cvec_buffer.data, &data, (cvec_buffer.len - byte_read));
        byte_read += cvec_buffer.len;

        CResult<CSv2Message, Sv2Error> frame = next_frame(decoder);

        switch (frame.tag) {
          case CResult < CSv2Message, Sv2Error > ::Tag::Ok:
            decoded = true;
            decoded_msg = frame.ok._0.new_template._0;
            break;
          case CResult < CSv2Message, Sv2Error > ::Tag::Err:
            std::cout << "DEBUG FUZZ TEST: TAG ERROR" << std::endl;
            decoded = true;
            break;
            // TODO: Assert the failure of decoding here
          };
    }

    free_decoder(decoder);
}
