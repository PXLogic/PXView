#ifndef PXVIEW_PV_DATA_IDECODER_HOST_H
#define PXVIEW_PV_DATA_IDECODER_HOST_H

#include <list>
#include <memory>
#include <vector>

struct srd_decoder;
class DecoderStatus;

namespace pv {
namespace data {

class DecoderStack;
class SessionDocument;
namespace decode { class Decoder; }

// IDecoderHost — decoder lifecycle management.
// Spec v2 Task 8: extracted from DataSource胖接口.
class IDecoderHost {
public:
    virtual ~IDecoderHost() = default;
    virtual bool add_decoder(srd_decoder *const dec, bool silent,
                             DecoderStatus *dstatus,
                             std::list<decode::Decoder *> &sub_decoders,
                             std::shared_ptr<DecoderStack> &out_stack,
                             SessionDocument *doc = nullptr) = 0;
    virtual void remove_decoder_by_key_handel(void *handel,
                                              SessionDocument *doc = nullptr) = 0;
    virtual void rst_decoder_by_key_handel(void *handel,
                                           SessionDocument *doc = nullptr) = 0;
    virtual void clear_all_decoder(bool bUpdateView = true) = 0;
    virtual void start_all_decode_tasks() = 0;
    virtual std::vector<std::shared_ptr<DecoderStack>>& get_decoder_stacks(
        SessionDocument *doc = nullptr) = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_IDECODER_HOST_H
