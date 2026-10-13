#include "graphics/shader/recompiler/ir/ResourcePlanEncoding.h"

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

// Every encoded struct is listed field by field below. A size change means a field was added
// or removed: update the matching Put/Get pair, then the size here.
static_assert(sizeof(MemoryInfo) == 72);
static_assert(sizeof(BufferResource) == 80);
static_assert(sizeof(ImageResource) == 88);
static_assert(sizeof(SamplerResource) == 48);
static_assert(sizeof(SampledResourcePair) == 12);
static_assert(sizeof(StageInput) == 56);
static_assert(sizeof(StageOutput) == 48);
static_assert(sizeof(ShaderInfo) == 240);
static_assert(sizeof(Bindings) == 248);
static_assert(sizeof(CompiledShaderInfo) == 528);
static_assert(sizeof(DescriptorSource) == 264);
static_assert(sizeof(DescriptorSource::IndirectDescriptor) == 120);
static_assert(sizeof(ResourceBlock) == 88);
static_assert(sizeof(SrtRead) == 24);
static_assert(sizeof(UniformFillPlan) == 96);
static_assert(sizeof(ResourcePlan) == 664);
static_assert(sizeof(ResourceSpecialization::Buffer) == 28);
static_assert(sizeof(ResourceSpecialization::Image) == 36);

class Writer {
public:
	explicit Writer(std::vector<uint8_t>& out): m_out(out) {}

	template <typename T>
	requires(std::is_trivially_copyable_v<T>)
	void Put(const T& value) {
		const auto offset = m_out.size();
		m_out.resize(offset + sizeof(T));
		std::memcpy(m_out.data() + offset, &value, sizeof(T));
	}

	void Size(size_t size) { Put(static_cast<uint32_t>(size)); }

	template <typename T>
	void Words(const std::vector<T>& values) {
		Size(values.size());
		for (const auto& value: values) Put(value);
	}

	void String(const std::string& text) {
		Size(text.size());
		m_out.insert(m_out.end(), text.begin(), text.end());
	}

private:
	std::vector<uint8_t>& m_out;
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t> bytes): m_bytes(bytes) {}

	template <typename T>
	requires(std::is_trivially_copyable_v<T>)
	bool Get(T& value) {
		if (m_bytes.size() - m_offset < sizeof(T)) return Fail();
		std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
		m_offset += sizeof(T);
		return true;
	}

	// A count of elements that are each at least `min_element_size` bytes. Rejects counts the
	// remaining bytes cannot hold, so malformed input never triggers a huge allocation.
	bool Size(size_t& size, size_t min_element_size = 1) {
		uint32_t count = 0;
		if (!Get(count)) return false;
		if (min_element_size != 0 && count > (m_bytes.size() - m_offset) / min_element_size) {
			return Fail();
		}
		size = count;
		return true;
	}

	template <typename T>
	bool Words(std::vector<T>& values) {
		size_t size = 0;
		if (!Size(size, sizeof(T))) return false;
		values.resize(size);
		for (auto& value: values) {
			if (!Get(value)) return false;
		}
		return true;
	}

	bool String(std::string& text) {
		size_t size = 0;
		if (!Size(size)) return false;
		text.assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset), size);
		m_offset += size;
		return true;
	}

	[[nodiscard]] bool Ok() const { return m_ok; }
	[[nodiscard]] bool AtEnd() const { return m_ok && m_offset == m_bytes.size(); }
	bool               Fail() {
		m_ok = false;
		return false;
	}

private:
	std::span<const uint8_t> m_bytes;
	size_t                   m_offset = 0;
	bool                     m_ok     = true;
};

// --- plain metadata -----------------------------------------------------------------------

void Put(Writer& w, const MemoryInfo& m) {
	w.Put(m.kind);
	w.Put(m.resource);
	w.Put(m.sampler);
	w.Put(m.offset);
	w.Put(m.secondary_offset);
	w.Put(m.dmask);
	w.Put(m.data_dwords);
	w.Put(m.data_bits);
	w.Put(m.component_index);
	w.Put(m.component_count);
	w.Put(m.data_format);
	w.Put(m.number_format);
	w.Put(m.image_sample_flags);
	w.Put(m.image_dimension);
	w.Put(m.image_address_components);
	w.Put(m.address_is_full);
	w.Put(m.data_signed);
	w.Put(m.typed);
	w.Put(m.formatted);
	w.Put(m.image_has_mip);
	w.Put(m.image_r128);
	w.Put(m.idxen);
	w.Put(m.offen);
	w.Put(m.coherent);
	w.Put(m.planning_only);
}

bool Get(Reader& r, MemoryInfo& m) {
	return r.Get(m.kind) && r.Get(m.resource) && r.Get(m.sampler) && r.Get(m.offset) &&
	       r.Get(m.secondary_offset) && r.Get(m.dmask) && r.Get(m.data_dwords) &&
	       r.Get(m.data_bits) && r.Get(m.component_index) && r.Get(m.component_count) &&
	       r.Get(m.data_format) && r.Get(m.number_format) && r.Get(m.image_sample_flags) &&
	       r.Get(m.image_dimension) && r.Get(m.image_address_components) &&
	       r.Get(m.address_is_full) && r.Get(m.data_signed) && r.Get(m.typed) &&
	       r.Get(m.formatted) && r.Get(m.image_has_mip) && r.Get(m.image_r128) &&
	       r.Get(m.idxen) && r.Get(m.offen) && r.Get(m.coherent) && r.Get(m.planning_only);
}

void Put(Writer& w, const BufferResource& b) {
	w.Put(b.source);
	w.Put(b.descriptor_index);
	w.Put(b.first_use_pc);
	w.Put(b.max_byte_extent);
	w.Put(b.packed_stride);
	w.Put(b.descriptor_format);
	w.Put(b.descriptor_swizzle);
	w.Put(b.image_alias);
	w.Put(b.read);
	w.Put(b.written);
	w.Put(b.atomic);
	w.Put(b.formatted);
	w.Put(b.scalar);
	w.Put(b.indirect_root);
	w.Put(b.indirect_mapping_offset);
	w.Put(b.indirect_search_iterations);
	w.Words(b.indirect_resources);
}

bool Get(Reader& r, BufferResource& b) {
	return r.Get(b.source) && r.Get(b.descriptor_index) && r.Get(b.first_use_pc) &&
	       r.Get(b.max_byte_extent) && r.Get(b.packed_stride) && r.Get(b.descriptor_format) &&
	       r.Get(b.descriptor_swizzle) && r.Get(b.image_alias) && r.Get(b.read) &&
	       r.Get(b.written) && r.Get(b.atomic) && r.Get(b.formatted) && r.Get(b.scalar) &&
	       r.Get(b.indirect_root) && r.Get(b.indirect_mapping_offset) &&
	       r.Get(b.indirect_search_iterations) && r.Words(b.indirect_resources);
}

void Put(Writer& w, const ImageResource& i) {
	w.Put(i.source);
	w.Put(i.descriptor_index);
	w.Put(i.first_use_pc);
	w.Put(i.resource_class);
	w.Put(i.numeric_class);
	w.Put(i.dimension);
	w.Put(i.mip_mode);
	w.Put(i.mip_count);
	w.Put(i.conversion_format);
	w.Put(i.shader_swizzle);
	w.Put(i.read);
	w.Put(i.written);
	w.Put(i.atomic);
	w.Put(i.atomic64);
	w.Put(i.depth_compare);
	w.Put(i.cube);
	w.Put(i.r128);
	w.Put(i.indirect_root);
	w.Put(i.indirect_mapping_offset);
	w.Put(i.indirect_search_iterations);
	w.Words(i.indirect_resources);
}

bool Get(Reader& r, ImageResource& i) {
	return r.Get(i.source) && r.Get(i.descriptor_index) && r.Get(i.first_use_pc) &&
	       r.Get(i.resource_class) && r.Get(i.numeric_class) && r.Get(i.dimension) &&
	       r.Get(i.mip_mode) && r.Get(i.mip_count) && r.Get(i.conversion_format) &&
	       r.Get(i.shader_swizzle) && r.Get(i.read) && r.Get(i.written) && r.Get(i.atomic) &&
	       r.Get(i.atomic64) && r.Get(i.depth_compare) && r.Get(i.cube) && r.Get(i.r128) &&
	       r.Get(i.indirect_root) && r.Get(i.indirect_mapping_offset) &&
	       r.Get(i.indirect_search_iterations) && r.Words(i.indirect_resources);
}

void Put(Writer& w, const SamplerResource& s) {
	w.Put(s.source);
	w.Put(s.first_use_pc);
	w.Put(s.snapshot_index);
	w.Words(s.indirect_resources);
	w.Put(s.force_point_filtering);
	w.Put(s.depth_compare);
	w.Put(s.integer_border);
	w.Put(s.gather_lod);
}

bool Get(Reader& r, SamplerResource& s) {
	return r.Get(s.source) && r.Get(s.first_use_pc) && r.Get(s.snapshot_index) &&
	       r.Words(s.indirect_resources) && r.Get(s.force_point_filtering) &&
	       r.Get(s.depth_compare) && r.Get(s.integer_border) && r.Get(s.gather_lod);
}

void Put(Writer& w, const StageInput& s) {
	w.Put(s.kind);
	w.Put(s.location);
	w.Put(s.component_count);
	w.String(s.debug_name);
	w.Put(s.per_vertex);
}

bool Get(Reader& r, StageInput& s) {
	return r.Get(s.kind) && r.Get(s.location) && r.Get(s.component_count) &&
	       r.String(s.debug_name) && r.Get(s.per_vertex);
}

void Put(Writer& w, const StageOutput& s) {
	w.Put(s.kind);
	w.Put(s.index);
	w.Put(s.location);
	w.String(s.debug_name);
}

bool Get(Reader& r, StageOutput& s) {
	return r.Get(s.kind) && r.Get(s.index) && r.Get(s.location) && r.String(s.debug_name);
}

template <typename T>
void PutList(Writer& w, const std::vector<T>& values) {
	w.Size(values.size());
	for (const auto& value: values) Put(w, value);
}

template <typename T>
bool GetList(Reader& r, std::vector<T>& values) {
	size_t size = 0;
	if (!r.Size(size)) return false;
	values.resize(size);
	for (auto& value: values) {
		if (!Get(r, value)) return false;
	}
	return true;
}

void Put(Writer& w, const ShaderInfo& s) {
	PutList(w, s.buffers);
	PutList(w, s.images);
	PutList(w, s.samplers);
	w.Words(s.sampled_pairs);
	PutList(w, s.inputs);
	PutList(w, s.outputs);
	w.Words(s.user_data_registers);
	w.Put(s.live_buffers);
	w.Put(s.vertex_fetch_components);
	w.Put(s.vertex_offset_sgpr);
	w.Put(s.instance_offset_sgpr);
	for (const bool flag: {s.has_bitwise_xor, s.uses_dma, s.uses_swizzle, s.uses_lds, s.uses_gds,
	                       s.uses_flattened_srt, s.uses_dispatch_threads, s.bvh, s.subgroup_ballot,
	                       s.subgroup_barrier, s.subgroup_shuffle, s.subgroup_local_invocation_id,
	                       s.compute_derivatives, s.image_gather_extended, s.function_lds,
	                       s.function_scratch, s.pixel_valid_mask, s.buffer_int64_atomics,
	                       s.buffer_u8, s.buffer_u16, s.shared_int64_atomics, s.coherent_buffers,
	                       s.float64}) {
		w.Put(flag);
	}
}

bool Get(Reader& r, ShaderInfo& s) {
	if (!GetList(r, s.buffers) || !GetList(r, s.images) || !GetList(r, s.samplers) ||
	    !r.Words(s.sampled_pairs) || !GetList(r, s.inputs) || !GetList(r, s.outputs) ||
	    !r.Words(s.user_data_registers) || !r.Get(s.live_buffers) ||
	    !r.Get(s.vertex_fetch_components) || !r.Get(s.vertex_offset_sgpr) ||
	    !r.Get(s.instance_offset_sgpr)) {
		return false;
	}
	for (bool* flag: {&s.has_bitwise_xor, &s.uses_dma, &s.uses_swizzle, &s.uses_lds, &s.uses_gds,
	                  &s.uses_flattened_srt, &s.uses_dispatch_threads, &s.bvh, &s.subgroup_ballot,
	                  &s.subgroup_barrier, &s.subgroup_shuffle, &s.subgroup_local_invocation_id,
	                  &s.compute_derivatives, &s.image_gather_extended, &s.function_lds,
	                  &s.function_scratch, &s.pixel_valid_mask, &s.buffer_int64_atomics,
	                  &s.buffer_u8, &s.buffer_u16, &s.shared_int64_atomics, &s.coherent_buffers,
	                  &s.float64}) {
		if (!r.Get(*flag)) return false;
	}
	return true;
}

void Put(Writer& w, const Bindings& b) {
	w.Put(b.descriptor_counts);
	w.Put(b.descriptor_mask);
	w.Put(b.push_data_start_dword);
	w.Put(b.dispatch_thread_dword);
	w.Put(b.memory_offset_dword);
}

bool Get(Reader& r, Bindings& b) {
	return r.Get(b.descriptor_counts) && r.Get(b.descriptor_mask) &&
	       r.Get(b.push_data_start_dword) && r.Get(b.dispatch_thread_dword) &&
	       r.Get(b.memory_offset_dword);
}

// --- IR values ------------------------------------------------------------------------------

// Instructions are referenced by their position in the plan's value_storage.
class ValueEncoder {
public:
	explicit ValueEncoder(const ResourcePlan& plan) {
		uint32_t index = 0;
		for (const auto& inst: plan.value_storage) m_index.emplace(&inst, index++);
	}

	bool Put(Writer& w, Value value) const {
		const auto type = value.IsImmediate() ? value.GetType() : Type::Opaque;
		w.Put(type);
		switch (type) {
			case Type::Void: return true;
			case Type::Opaque: {
				const auto found = m_index.find(value.TryInstruction());
				if (found == m_index.end()) return false;
				w.Put(found->second);
				return true;
			}
			case Type::ScalarReg: w.Put(value.ScalarRegister()); return true;
			case Type::VectorReg: w.Put(value.VectorRegister()); return true;
			case Type::U1: w.Put(value.U1()); return true;
			case Type::U8: w.Put(value.U8()); return true;
			case Type::U16: w.Put(value.U16()); return true;
			case Type::F16: w.Put(value.F16Bits()); return true;
			case Type::U32: w.Put(value.U32()); return true;
			case Type::F32: w.Put(std::bit_cast<uint32_t>(value.F32Value())); return true;
			case Type::U64: w.Put(value.U64()); return true;
			default: return false;
		}
	}

private:
	std::unordered_map<const Inst*, uint32_t> m_index;
};

class ValueDecoder {
public:
	explicit ValueDecoder(ResourcePlan& plan) {
		for (auto& inst: plan.value_storage) m_insts.push_back(&inst);
	}

	bool Get(Reader& r, Value& value) const {
		Type type = Type::Void;
		if (!r.Get(type)) return false;
		switch (type) {
			case Type::Void: value = {}; return true;
			case Type::Opaque: {
				uint32_t index = 0;
				if (!r.Get(index) || index >= m_insts.size()) return r.Fail();
				value = Value(m_insts[index]);
				return true;
			}
			case Type::ScalarReg: return Immediate<ScalarReg>(r, value);
			case Type::VectorReg: return Immediate<VectorReg>(r, value);
			case Type::U1: return Immediate<bool>(r, value);
			case Type::U8: return Immediate<uint8_t>(r, value);
			case Type::U16: return Immediate<uint16_t>(r, value);
			case Type::F16: {
				uint16_t bits = 0;
				if (!r.Get(bits)) return false;
				value = Value::F16(bits);
				return true;
			}
			case Type::U32: return Immediate<uint32_t>(r, value);
			case Type::F32: {
				uint32_t bits = 0;
				if (!r.Get(bits)) return false;
				value = Value::F32(std::bit_cast<float>(bits));
				return true;
			}
			case Type::U64: return Immediate<uint64_t>(r, value);
			default: return r.Fail();
		}
	}

private:
	template <typename T>
	static bool Immediate(Reader& r, Value& value) {
		T raw {};
		if (!r.Get(raw)) return false;
		value = Value(raw);
		return true;
	}

	std::vector<Inst*> m_insts;
};

bool PutInstructions(Writer& w, const ResourcePlan& plan, const ValueEncoder& values) {
	w.Size(plan.value_storage.size());
	for (const auto& inst: plan.value_storage) {
		w.Put(inst.GetOpcode());
		w.Put(inst.Flags<uint64_t>());
	}
	for (const auto& inst: plan.value_storage) {
		w.Size(inst.NumArgs());
		for (size_t i = 0; i < inst.NumArgs(); i++) {
			if (!values.Put(w, inst.Arg(i))) return false;
		}
	}
	return true;
}

bool GetInstructions(Reader& r, ResourcePlan& plan) {
	size_t count = 0;
	if (!r.Size(count, sizeof(ValueOpcode) + sizeof(uint64_t))) return false;
	for (size_t i = 0; i < count; i++) {
		ValueOpcode opcode {};
		uint64_t    flags = 0;
		if (!r.Get(opcode) || !r.Get(flags)) return false;
		if (static_cast<uint32_t>(opcode) >= static_cast<uint32_t>(ValueOpcode::Count)) {
			return r.Fail();
		}
		plan.value_storage.emplace_back(opcode, flags);
	}
	const ValueDecoder values(plan);
	for (auto& inst: plan.value_storage) {
		size_t args = 0;
		if (!r.Size(args, sizeof(Type))) return false;
		const bool phi = inst.GetOpcode() == ValueOpcode::Phi;
		if (!phi && args != inst.NumArgs()) return r.Fail();
		for (size_t i = 0; i < args; i++) {
			Value arg;
			if (!values.Get(r, arg)) return false;
			if (phi) {
				inst.AddPhiOperand(nullptr, arg);
			} else {
				inst.SetArg(i, arg);
			}
		}
	}
	return true;
}

// --- plan structures holding IR values ----------------------------------------------------------

bool Put(Writer& w, const ValueEncoder& v, const DescriptorSource& s) {
	w.Put(s.dword_count);
	for (const auto& dword: s.dwords) {
		if (!v.Put(w, dword)) return false;
	}
	w.Put(s.indirect_descriptor.has_value());
	if (!s.indirect_descriptor.has_value()) return true;
	const auto& d = *s.indirect_descriptor;
	w.Put(d.selector.has_value());
	if (d.selector.has_value()) {
		w.Put(d.selector->source);
		w.Put(d.selector->stride);
		w.Put(d.selector->offset);
	}
	w.Put(d.table_source);
	w.Put(d.table_offset);
	w.Put(d.table_immediate);
	w.Put(d.table_stride);
	w.Put(d.table_record_bytes);
	w.Put(d.table_scalar);
	w.Put(d.workgroup_axis);
	w.Words(d.sources);
	return v.Put(w, d.key_count) && v.Put(w, d.selector_first) && v.Put(w, d.selector_mask);
}

bool Get(Reader& r, const ValueDecoder& v, DescriptorSource& s) {
	if (!r.Get(s.dword_count)) return false;
	for (auto& dword: s.dwords) {
		if (!v.Get(r, dword)) return false;
	}
	bool indirect = false;
	if (!r.Get(indirect)) return false;
	if (!indirect) return true;
	auto& d        = s.indirect_descriptor.emplace();
	bool  selector = false;
	if (!r.Get(selector)) return false;
	if (selector) {
		auto& read = d.selector.emplace();
		if (!r.Get(read.source) || !r.Get(read.stride) || !r.Get(read.offset)) return false;
	}
	return r.Get(d.table_source) && r.Get(d.table_offset) && r.Get(d.table_immediate) &&
	       r.Get(d.table_stride) && r.Get(d.table_record_bytes) && r.Get(d.table_scalar) &&
	       r.Get(d.workgroup_axis) && r.Words(d.sources) && v.Get(r, d.key_count) &&
	       v.Get(r, d.selector_first) && v.Get(r, d.selector_mask);
}

bool Put(Writer& w, const ValueEncoder& v, const ResourceBlock& b) {
	w.Words(b.successors);
	w.Words(b.sources);
	w.Words(b.srt_reads);
	return v.Put(w, b.condition);
}

bool Get(Reader& r, const ValueDecoder& v, ResourceBlock& b) {
	return r.Words(b.successors) && r.Words(b.sources) && r.Words(b.srt_reads) &&
	       v.Get(r, b.condition);
}

bool Put(Writer& w, const ValueEncoder& v, const SrtRead& s) {
	w.Put(s.flat_offset);
	return v.Put(w, s.value);
}

bool Get(Reader& r, const ValueDecoder& v, SrtRead& s) {
	return r.Get(s.flat_offset) && v.Get(r, s.value);
}

template <typename T>
bool PutValueList(Writer& w, const ValueEncoder& v, const std::vector<T>& values) {
	w.Size(values.size());
	for (const auto& value: values) {
		if (!Put(w, v, value)) return false;
	}
	return true;
}

template <typename T>
bool GetValueList(Reader& r, const ValueDecoder& v, std::vector<T>& values) {
	size_t size = 0;
	if (!r.Size(size)) return false;
	values.resize(size);
	for (auto& value: values) {
		if (!Get(r, v, value)) return false;
	}
	return true;
}

bool GetReads(Reader& r, std::vector<std::pair<uint64_t, uint64_t>>& reads) {
	size_t size = 0;
	if (!r.Size(size, 2 * sizeof(uint64_t))) return false;
	reads.resize(size);
	for (auto& [address, bytes]: reads) {
		if (!r.Get(address) || !r.Get(bytes)) return false;
	}
	return true;
}

} // namespace

bool EncodeResourcePlan(const ResourcePlan& plan, std::vector<uint8_t>& out) {
	std::vector<uint8_t> bytes;
	Writer               w(bytes);
	const ValueEncoder   values(plan);
	w.Put(plan.stage);
	w.Put(plan.shader_hash);
	w.Put(plan.user_data_base);
	w.Put(plan.user_data_count);
	if (!PutInstructions(w, plan, values)) return false;
	PutList(w, plan.memory_info);
	if (!PutValueList(w, values, plan.descriptor_sources) ||
	    !PutValueList(w, values, plan.control_flow) || !PutValueList(w, values, plan.srt_reads)) {
		return false;
	}
	w.Put(plan.requires_specialization_memory);
	w.Size(plan.source_reads.size());
	for (const auto& [address, size]: plan.source_reads) {
		w.Put(address);
		w.Put(size);
	}
	w.Put(plan.capture_specialization_reads);
	w.Put(plan.srt_plan_complete);
	w.Put(plan.resource_tracking_complete);
	Put(w, plan.info);
	w.Put(plan.uniform_fill.fill);
	for (const auto& value: plan.uniform_fill.values) {
		if (!values.Put(w, value)) return false;
	}
	out.insert(out.end(), bytes.begin(), bytes.end());
	return true;
}

bool DecodeResourcePlan(std::span<const uint8_t> bytes, ResourcePlan& plan) {
	Reader r(bytes);
	if (!r.Get(plan.stage) || !r.Get(plan.shader_hash) || !r.Get(plan.user_data_base) ||
	    !r.Get(plan.user_data_count) || !GetInstructions(r, plan)) {
		return false;
	}
	const ValueDecoder values(plan);
	if (!GetList(r, plan.memory_info) || !GetValueList(r, values, plan.descriptor_sources) ||
	    !GetValueList(r, values, plan.control_flow) || !GetValueList(r, values, plan.srt_reads) ||
	    !r.Get(plan.requires_specialization_memory) || !GetReads(r, plan.source_reads) ||
	    !r.Get(plan.capture_specialization_reads) || !r.Get(plan.srt_plan_complete) ||
	    !r.Get(plan.resource_tracking_complete) || !Get(r, plan.info) ||
	    !r.Get(plan.uniform_fill.fill)) {
		return false;
	}
	for (auto& value: plan.uniform_fill.values) {
		if (!values.Get(r, value)) return false;
	}
	if (!r.AtEnd()) return false;
	// The walker is generated machine code; it is rebuilt from the plan, as at translation.
	CompileSrtWalker(plan);
	return true;
}

void EncodeCompiledShaderInfo(const CompiledShaderInfo& info, std::vector<uint8_t>& out) {
	Writer w(out);
	w.Put(info.stage);
	w.Put(info.shader_hash);
	w.Put(info.wave_size);
	w.Put(info.user_data_base);
	w.Put(info.user_data_count);
	w.Put(info.scratch_dwords);
	w.Put(info.param_export_mask);
	w.Put(info.has_address_writes);
	Put(w, info.info);
	Put(w, info.bindings);
}

bool DecodeCompiledShaderInfo(std::span<const uint8_t> bytes, CompiledShaderInfo& info) {
	Reader r(bytes);
	return r.Get(info.stage) && r.Get(info.shader_hash) && r.Get(info.wave_size) &&
	       r.Get(info.user_data_base) && r.Get(info.user_data_count) &&
	       r.Get(info.scratch_dwords) && r.Get(info.param_export_mask) &&
	       r.Get(info.has_address_writes) && Get(r, info.info) && Get(r, info.bindings) &&
	       r.AtEnd();
}

void EncodeSpecialization(const ResourceSpecialization& specialization, std::vector<uint8_t>& out) {
	Writer w(out);
	w.Size(specialization.buffers.size());
	for (const auto& b: specialization.buffers) {
		w.Put(b.packed_stride);
		w.Put(b.descriptor_format);
		w.Put(b.descriptor_swizzle);
		w.Put(b.zero_stride_oob);
		w.Put(b.indirect_root);
		w.Put(b.indirect_mapping_offset);
		w.Put(b.indirect_search_iterations);
	}
	w.Size(specialization.images.size());
	for (const auto& i: specialization.images) {
		w.Put(i.numeric_class);
		w.Put(i.dimension);
		w.Put(i.mip_count);
		w.Put(i.conversion_format);
		w.Put(i.shader_swizzle);
		w.Put(i.indirect_root);
		w.Put(i.indirect_mapping_offset);
		w.Put(i.indirect_search_iterations);
		w.Put(i.cube);
		w.Put(i.fmask);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
