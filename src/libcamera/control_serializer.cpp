/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * control_serializer.cpp - Control (de)serializer
 */

#include "control_serializer.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <ipa/ipa_controls.h>
#include <libcamera/control_ids.h>
#include <libcamera/controls.h>

#include "byte_stream_buffer.h"
#include "log.h"

/**
 * \file control_serializer.h
 * \brief Serialization and deserialization helpers for controls
 */

namespace libcamera {

LOG_DEFINE_CATEGORY(Serializer)

namespace {

static constexpr size_t ControlValueSize[] = {
	[ControlTypeNone]	= 1,
	[ControlTypeBool]	= sizeof(bool),
	[ControlTypeInteger32]	= sizeof(int32_t),
	[ControlTypeInteger64]	= sizeof(int64_t),
};

} /* namespace */

/**
 * \class ControlSerializer
 * \brief Serializer and deserializer for control-related classes
 *
 * The control serializer is a helper to serialize and deserialize
 * ControlInfoMap and ControlValue instances for the purpose of communication
 * with IPA modules.
 *
 * Neither the ControlInfoMap nor the ControlList are self-contained data
 * container. ControlInfoMap references an external ControlId in each of its
 * entries, and ControlList references a ControlInfoMap for the purpose of
 * validation. Serializing and deserializing those objects thus requires a
 * context that maintains the associations between them. The control serializer
 * fulfils this task.
 *
 * ControlInfoMap instances can be serialized on their own, but require
 * ControlId instances to be provided at deserialization time. The serializer
 * recreates those ControlId instances and stores them in an internal cache,
 * from which the ControlInfoMap is populated.
 *
 * ControlList instances need to be associated with a ControlInfoMap when
 * deserialized. To make this possible, the control lists are serialized with a
 * handle to their ControlInfoMap, and the map is looked up from the handle at
 * deserialization time. To make this possible, the serializer assigns a
 * numerical handle to ControlInfoMap instances when they are serialized, and
 * stores the mapping between handle and ControlInfoMap both when serializing
 * (for the pipeline handler side) and deserializing (for the IPA side) them.
 * This mapping is used when serializing a ControlList to include the
 * corresponding ControlInfoMap handle in the binary data, and when
 * deserializing to retrieve the corresponding ControlInfoMap.
 *
 * In order to perform those tasks, the serializer keeps an internal state that
 * needs to be properly populated. This mechanism requires the ControlInfoMap
 * corresponding to a ControlList to have been serialized or deserialized
 * before the ControlList is serialized or deserialized. Failure to comply with
 * that constraint results in serialization or deserialization failure of the
 * ControlList.
 *
 * The serializer can be reset() to clear its internal state. This may be
 * performed when reconfiguring an IPA to avoid constant growth of the internal
 * state, especially if the contents of the ControlInfoMap instances change at
 * that time. A reset of the serializer invalidates all ControlList and
 * ControlInfoMap that have been previously deserialized. The caller shall thus
 * proceed with care to avoid stale references.
 */

ControlSerializer::ControlSerializer()
	: serial_(0)
{
}

/**
 * \brief Reset the serializer
 *
 * Reset the internal state of the serializer. This invalidates all the
 * ControlList and ControlInfoMap that have been previously deserialized.
 */
void ControlSerializer::reset()
{
	serial_ = 0;

	infoMapHandles_.clear();
	infoMaps_.clear();
	controlIds_.clear();
}

size_t ControlSerializer::binarySize(const ControlValue &value)
{
	return ControlValueSize[value.type()];
}

size_t ControlSerializer::binarySize(const ControlRange &range)
{
	return binarySize(range.min()) + binarySize(range.max());
}

/**
 * \brief Retrieve the size in bytes required to serialize a ControlInfoMap
 * \param[in] info The control info map
 *
 * Compute and return the size in bytes required to store the serialized
 * ControlInfoMap.
 *
 * \return The size in bytes required to store the serialized ControlInfoMap
 */
size_t ControlSerializer::binarySize(const ControlInfoMap &info)
{
	size_t size = sizeof(struct ipa_controls_header)
		    + info.size() * sizeof(struct ipa_control_range_entry);

	for (const auto &ctrl : info)
		size += binarySize(ctrl.second);

	return size;
}

/**
 * \brief Retrieve the size in bytes required to serialize a ControlList
 * \param[in] list The control list
 *
 * Compute and return the size in bytes required to store the serialized
 * ControlList.
 *
 * \return The size in bytes required to store the serialized ControlList
 */
size_t ControlSerializer::binarySize(const ControlList &list)
{
	size_t size = sizeof(struct ipa_controls_header)
		    + list.size() * sizeof(struct ipa_control_value_entry);

	for (const auto &ctrl : list)
		size += binarySize(ctrl.second);

	return size;
}

void ControlSerializer::store(const ControlValue &value,
			      ByteStreamBuffer &buffer)
{
	switch (value.type()) {
	case ControlTypeBool: {
		bool data = value.get<bool>();
		buffer.write(&data);
		break;
	}

	case ControlTypeInteger32: {
		int32_t data = value.get<int32_t>();
		buffer.write(&data);
		break;
	}

	case ControlTypeInteger64: {
		uint64_t data = value.get<int64_t>();
		buffer.write(&data);
		break;
	}

	default:
		break;
	}
}

void ControlSerializer::store(const ControlRange &range,
			      ByteStreamBuffer &buffer)
{
	store(range.min(), buffer);
	store(range.max(), buffer);
}

/**
 * \brief Serialize a ControlInfoMap in a buffer
 * \param[in] info The control info map to serialize
 * \param[in] buffer The memory buffer where to serialize the ControlInfoMap
 *
 * Serialize the \a info map into the \a buffer using the serialization format
 * defined by the IPA context interface in ipa_controls.h.
 *
 * The serializer stores a reference to the \a info internally. The caller
 * shall ensure that \a info stays valid until the serializer is reset().
 *
 * \return 0 on success, a negative error code otherwise
 * \retval -ENOSPC Not enough space is available in the buffer
 */
int ControlSerializer::serialize(const ControlInfoMap &info,
				 ByteStreamBuffer &buffer)
{
	/* Compute entries and data required sizes. */
	size_t entriesSize = info.size() * sizeof(struct ipa_control_range_entry);
	size_t valuesSize = 0;
	for (const auto &ctrl : info)
		valuesSize += binarySize(ctrl.second);

	/* Prepare the packet header, assign a handle to the ControlInfoMap. */
	struct ipa_controls_header hdr;
	hdr.version = IPA_CONTROLS_FORMAT_VERSION;
	hdr.handle = ++serial_;
	hdr.entries = info.size();
	hdr.size = sizeof(hdr) + entriesSize + valuesSize;
	hdr.data_offset = sizeof(hdr) + entriesSize;

	buffer.write(&hdr);

	/*
	 * Serialize all entries.
	 * \todo Serialize the control name too
	 */
	ByteStreamBuffer entries = buffer.carveOut(entriesSize);
	ByteStreamBuffer values = buffer.carveOut(valuesSize);

	for (const auto &ctrl : info) {
		const ControlId *id = ctrl.first;
		const ControlRange &range = ctrl.second;

		struct ipa_control_range_entry entry;
		entry.id = id->id();
		entry.type = id->type();
		entry.offset = values.offset();
		entries.write(&entry);

		store(range, values);
	}

	if (buffer.overflow())
		return -ENOSPC;

	/*
	 * Store the map to handle association, to be used to serialize and
	 * deserialize control lists.
	 */
	infoMapHandles_[&info] = hdr.handle;

	return 0;
}

/**
 * \brief Serialize a ControlList in a buffer
 * \param[in] list The control list to serialize
 * \param[in] buffer The memory buffer where to serialize the ControlList
 *
 * Serialize the \a list into the \a buffer using the serialization format
 * defined by the IPA context interface in ipa_controls.h.
 *
 * \return 0 on success, a negative error code otherwise
 * \retval -ENOENT The ControlList is related to an unknown ControlInfoMap
 * \retval -ENOSPC Not enough space is available in the buffer
 */
int ControlSerializer::serialize(const ControlList &list,
				 ByteStreamBuffer &buffer)
{
	/*
	 * Find the ControlInfoMap handle for the ControlList if it has one, or
	 * use 0 for ControlList without a ControlInfoMap.
	 */
	unsigned int infoMapHandle;
	if (list.infoMap()) {
		auto iter = infoMapHandles_.find(list.infoMap());
		if (iter == infoMapHandles_.end()) {
			LOG(Serializer, Error)
				<< "Can't serialize ControlList: unknown ControlInfoMap";
			return -ENOENT;
		}

		infoMapHandle = iter->second;
	} else {
		infoMapHandle = 0;
	}

	size_t entriesSize = list.size() * sizeof(struct ipa_control_value_entry);
	size_t valuesSize = 0;
	for (const auto &ctrl : list)
		valuesSize += binarySize(ctrl.second);

	/* Prepare the packet header. */
	struct ipa_controls_header hdr;
	hdr.version = IPA_CONTROLS_FORMAT_VERSION;
	hdr.handle = infoMapHandle;
	hdr.entries = list.size();
	hdr.size = sizeof(hdr) + entriesSize + valuesSize;
	hdr.data_offset = sizeof(hdr) + entriesSize;

	buffer.write(&hdr);

	ByteStreamBuffer entries = buffer.carveOut(entriesSize);
	ByteStreamBuffer values = buffer.carveOut(valuesSize);

	/* Serialize all entries. */
	for (const auto &ctrl : list) {
		unsigned int id = ctrl.first;
		const ControlValue &value = ctrl.second;

		struct ipa_control_value_entry entry;
		entry.id = id;
		entry.count = 1;
		entry.type = value.type();
		entry.offset = values.offset();
		entries.write(&entry);

		store(value, values);
	}

	if (buffer.overflow())
		return -ENOSPC;

	return 0;
}

template<>
ControlValue ControlSerializer::load<ControlValue>(ControlType type,
						   ByteStreamBuffer &b)
{
	switch (type) {
	case ControlTypeBool: {
		bool value;
		b.read(&value);
		return ControlValue(value);
	}

	case ControlTypeInteger32: {
		int32_t value;
		b.read(&value);
		return ControlValue(value);
	}

	case ControlTypeInteger64: {
		int64_t value;
		b.read(&value);
		return ControlValue(value);
	}

	default:
		return ControlValue();
	}
}

template<>
ControlRange ControlSerializer::load<ControlRange>(ControlType type,
						   ByteStreamBuffer &b)
{
	ControlValue min = load<ControlValue>(type, b);
	ControlValue max = load<ControlValue>(type, b);

	return ControlRange(min, max);
}

/**
 * \fn template<typename T> T ControlSerializer::deserialize(ByteStreamBuffer &buffer)
 * \brief Deserialize an object from a binary buffer
 * \param[in] buffer The memory buffer that contains the object
 *
 * This method is only valid when specialized for ControlInfoMap or
 * ControlList. Any other typename \a T is not supported.
 */

/**
 * \brief Deserialize a ControlInfoMap from a binary buffer
 * \param[in] buffer The memory buffer that contains the serialized map
 *
 * Re-construct a ControlInfoMap from a binary \a buffer containing data
 * serialized using the serialize() method.
 *
 * \return The deserialized ControlInfoMap
 */
template<>
ControlInfoMap ControlSerializer::deserialize<ControlInfoMap>(ByteStreamBuffer &buffer)
{
	struct ipa_controls_header hdr;
	buffer.read(&hdr);

	if (hdr.version != IPA_CONTROLS_FORMAT_VERSION) {
		LOG(Serializer, Error)
			<< "Unsupported controls format version "
			<< hdr.version;
		return {};
	}

	ByteStreamBuffer entries = buffer.carveOut(hdr.data_offset - sizeof(hdr));
	ByteStreamBuffer values = buffer.carveOut(hdr.size - hdr.data_offset);

	if (buffer.overflow()) {
		LOG(Serializer, Error) << "Serialized packet too small";
		return {};
	}

	ControlInfoMap::Map ctrls;

	for (unsigned int i = 0; i < hdr.entries; ++i) {
		struct ipa_control_range_entry entry;
		entries.read(&entry);

		/* Create and cache the individual ControlId. */
		ControlType type = static_cast<ControlType>(entry.type);
		/**
		 * \todo Find a way to preserve the control name for debugging
		 * purpose.
		 */
		controlIds_.emplace_back(utils::make_unique<ControlId>(entry.id, "", type));

		if (entry.offset != values.offset()) {
			LOG(Serializer, Error)
				<< "Bad data, entry offset mismatch (entry "
				<< i << ")";
			return {};
		}

		/* Create and store the ControlRange. */
		ctrls.emplace(controlIds_.back().get(),
			      load<ControlRange>(type, values));
	}

	/*
	 * Create the ControlInfoMap in the cache, and store the map to handle
	 * association.
	 */
	ControlInfoMap &map = infoMaps_[hdr.handle] = std::move(ctrls);
	infoMapHandles_[&map] = hdr.handle;

	return map;
}

/**
 * \brief Deserialize a ControlList from a binary buffer
 * \param[in] buffer The memory buffer that contains the serialized list
 *
 * Re-construct a ControlList from a binary \a buffer containing data
 * serialized using the serialize() method.
 *
 * \return The deserialized ControlList
 */
template<>
ControlList ControlSerializer::deserialize<ControlList>(ByteStreamBuffer &buffer)
{
	struct ipa_controls_header hdr;
	buffer.read(&hdr);

	if (hdr.version != IPA_CONTROLS_FORMAT_VERSION) {
		LOG(Serializer, Error)
			<< "Unsupported controls format version "
			<< hdr.version;
		return {};
	}

	ByteStreamBuffer entries = buffer.carveOut(hdr.data_offset - sizeof(hdr));
	ByteStreamBuffer values = buffer.carveOut(hdr.size - hdr.data_offset);

	if (buffer.overflow()) {
		LOG(Serializer, Error) << "Serialized packet too small";
		return {};
	}

	/*
	 * Retrieve the ControlInfoMap associated with the ControlList based on
	 * its ID. The mapping between infoMap and ID is set up when serializing
	 * or deserializing ControlInfoMap. If no mapping is found (which is
	 * currently the case for ControlList related to libcamera controls),
	 * use the global control::control idmap.
	 */
	const ControlInfoMap *infoMap;
	if (hdr.handle) {
		auto iter = std::find_if(infoMapHandles_.begin(), infoMapHandles_.end(),
					 [&](decltype(infoMapHandles_)::value_type &entry) {
						 return entry.second == hdr.handle;
					 });
		if (iter == infoMapHandles_.end()) {
			LOG(Serializer, Error)
				<< "Can't deserialize ControlList: unknown ControlInfoMap";
			return {};
		}

		infoMap = iter->first;
	} else {
		infoMap = nullptr;
	}

	ControlList ctrls(infoMap ? infoMap->idmap() : controls::controls);

	for (unsigned int i = 0; i < hdr.entries; ++i) {
		struct ipa_control_value_entry entry;
		entries.read(&entry);

		if (entry.offset != values.offset()) {
			LOG(Serializer, Error)
				<< "Bad data, entry offset mismatch (entry "
				<< i << ")";
			return {};
		}

		ControlType type = static_cast<ControlType>(entry.type);
		ctrls.set(entry.id, load<ControlValue>(type, values));
	}

	return ctrls;
}

} /* namespace libcamera */