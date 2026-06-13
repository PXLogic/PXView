#include <libsigrokdecode.h>
#include "decoder.h"
#include <assert.h>
#include "../../log.h"

namespace pv {
namespace data {
namespace decode {

Decoder::Decoder(const srd_decoder *const dec):
    _decoder(dec)
{
	_shown = true;
	_decode_start = 0;
	_decode_end = 0;
}

Decoder::~Decoder()
{
    for (auto i = _options.begin(); i != _options.end(); i++){
        if ((*i).second)
            g_variant_unref((*i).second);
	}
}
  
void Decoder::set_probes(std::map<const srd_channel*, int> probes)
{
    _probes = probes;
}
  
void Decoder::set_option(const char *id, GVariant *value)
{
	assert(value);
    if (_options[id]) {
        g_variant_unref(_options[id]);
    }
	g_variant_ref_sink(value);
    _options[id] = value;
}

void Decoder::set_decode_region(uint64_t start, uint64_t end)
{
    _decode_start = start;
    _decode_end = end;
}
  
bool Decoder::commit()
{
    return true;
}

int Decoder::first_probe_index()
{
	auto it = _probes.begin();
	if (it != _probes.end()){
		return (*it).second;
	}

	return -1;
}

bool Decoder::is_binded_probe(const srd_channel *const pdch)
{
	assert(pdch);

	auto it = _probes.find(pdch);
	return it != _probes.end();
}

int Decoder::binded_probe_index(const srd_channel *const pdch)
{
	assert(pdch);

	auto it = _probes.find(pdch);
	if (it != _probes.end()){
		return (*it).second;
	}

	return -1;
}

std::vector<const srd_channel*> Decoder::binded_probe_list()
{
	std::vector<const srd_channel*> lst;

	for (auto it = _probes.begin(); it != _probes.end(); ++it){ 
		lst.push_back((*it).first);
	}

	return lst;
}

bool Decoder::have_required_probes()
{
	pxv_info("decoder:%p", this);

	for (GSList *l = _decoder->channels; l; l = l->next) {
		const srd_channel *const pdch = (const srd_channel*)l->data;
		pxv_info("base decoder:%p", (void*)pdch);
	}

	for (auto it = _probes.begin(); it != _probes.end(); ++it){
		const srd_channel *const pdch = (const srd_channel*)(*it).first;
		pxv_info("got decoder:%p", (void*)pdch);
	}

	for (GSList *l = _decoder->channels; l; l = l->next) {
		const srd_channel *const pdch = (const srd_channel*)l->data;
		assert(pdch);
		if (_probes.find(pdch) == _probes.end())
			return false;
	}

	return true;
}

srd_decoder_inst* Decoder::create_decoder_inst(srd_session *session)
{
	GHashTable *const opt_hash = g_hash_table_new_full(g_str_hash,
		g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

	for (auto i = _options.begin(); i != _options.end(); i++)
	{
		GVariant *const value = (*i).second;
		g_variant_ref(value);
		g_hash_table_replace(opt_hash, (void*)g_strdup(
			(*i).first.c_str()), value);
	}

	srd_decoder_inst *const decoder_inst = srd_inst_new(
		session, _decoder->id, opt_hash);
	g_hash_table_destroy(opt_hash);

	if(!decoder_inst)
		return NULL;

	GHashTable *const probes = g_hash_table_new_full(g_str_hash,
		g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

    for(auto it = _probes.begin(); it != _probes.end(); it++)
	{
        GVariant *const gvar = g_variant_new_int32((*it).second);
		g_variant_ref_sink(gvar);
		g_hash_table_insert(probes, (*it).first->id, gvar);
	}

    srd_inst_channel_set_all(decoder_inst, probes);

	return decoder_inst;
}

} // decode
} // data
} // pv
