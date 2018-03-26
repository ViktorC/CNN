/*
 * DataProvider.h
 *
 *  Created on: 18.01.2018
 *      Author: Viktor Csomor
 */

#ifndef DATAPROVIDER_H_
#define DATAPROVIDER_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "Dimensions.h"
#include "Utils.h"

namespace cattle {

// TODO Specialized data providers for the MNIST, CIFAR, and ImageNet data sets.

/**
 * An alias for a unique tensor pointer.
 */
template<typename Scalar, std::size_t Rank>
using TensorPtr = std::unique_ptr<Tensor<Scalar,Rank>>;

/**
 * An alias for a pair of two tensors of the same rank. It represents observation-objective pairs.
 */
template<typename Scalar, std::size_t Rank, bool Sequential>
using DataPair = std::pair<Tensor<Scalar,Rank + Sequential + 1>,Tensor<Scalar,Rank + Sequential + 1>>;

/**
 * A class template for fetching data from memory or disk.
 */
template<typename Scalar, std::size_t Rank, bool Sequential>
class DataProvider {
	static_assert(std::is_floating_point<Scalar>::value, "non floating-point scalar type");
	static_assert(Rank > 0 && Rank < 4, "illegal data provider rank");
public:
	virtual ~DataProvider() = default;
	/**
	 * A simple constant getter method for the dimensions of the observations.
	 *
	 * @return A constant reference to the dimensions of the observations.
	 */
	virtual const Dimensions<std::size_t,Rank>& get_obs_dims() const = 0;
	/**
	 * A simple constant getter method for the dimensions of the objectives.
	 *
	 * @return A constant reference to the dimensions of the objectives.
	 */
	virtual const Dimensions<std::size_t,Rank>& get_obj_dims() const = 0;
	/**
	 * A simple constant method that returns whether the data provider instance has more
	 * data to provide.
	 *
	 * @return Whether there are more observation-objective pairs to read from the
	 * instance.
	 */
	virtual bool has_more() const = 0;
	/**
	 * Reads and returns the specified number of observation-objective pairs. It also
	 * offsets the reader by the specified number.
	 *
	 * @param batch_size The maximum number of observation-objective pairs to read and
	 * return.
	 * @return At most batch_size number of observation-objective pairs. If the number
	 * of unread pairs is less than batch_size, the number of returned pairs is that
	 * of the unread ones.
	 */
	virtual DataPair<Scalar,Rank,Sequential> get_data(std::size_t batch_size) = 0;
	/**
	 * It resets the reader head to the beginning of the data storage.
	 */
	virtual void reset() = 0;
protected:
	/**
	 * It skips the specified number of data points.
	 *
	 * @param instances The number of instances to skip.
	 */
	virtual void skip(std::size_t instances) = 0;
protected:
	static constexpr std::size_t DATA_RANKS = Rank + Sequential + 1;
	typedef Tensor<Scalar,DATA_RANKS> Data;
	typedef TensorPtr<Scalar,DATA_RANKS> DataPtr;
};

/**
 * A wrapper class template for data providers associated with continuous partitions of other data
 * providers. It enables the partitioning of a data provider into training and test data providers
 * by mapping two contiguous blocks of its data to two PartitionedDataProvider instances.
 */
template<typename Scalar, std::size_t Rank, bool Sequential>
class PartitionDataProvider : DataProvider<Scalar,Rank,Sequential> {
	typedef DataProvider<Scalar,Rank,Sequential> Base;
public:
	inline PartitionDataProvider(Base& orig_provider, std::size_t offset, std::size_t length) :
			orig_provider(orig_provider),
			offset(offset),
			length(length) {
		assert(length > 0);
		reset();
	}
	inline const Dimensions<std::size_t,Rank>& get_obs_dims() const {
		return orig_provider.get_obs_dims();
	}
	inline const Dimensions<std::size_t,Rank>& get_obj_dims() const {
		return orig_provider.get_obj_dims();
	}
	inline bool has_more() const {
		return instances_read < length && orig_provider.has_more();
	}
	inline DataPair<Scalar,Rank,Sequential> get_data(std::size_t batch_size) {
		std::size_t instances_to_read = std::min(batch_size, length - instances_read);
		instances_read += instances_to_read;
		return orig_provider.get_data(instances_to_read);
	}
	inline void reset() {
		orig_provider.reset();
		orig_provider.skip(offset);
		instances_read = 0;
	}
protected:
	inline void skip(std::size_t instances) {
		orig_provider.skip(instances);
	}
private:
	Base& orig_provider;
	const std::size_t offset;
	const std::size_t length;
	std::size_t instances_read;
};


/**
 * A data provider that reads from the memory. It requires the entire observation and
 * objective data sets to be loaded into memory, but it fetches pairs faster.
 */
template<typename Scalar, std::size_t Rank, bool Sequential>
class MemoryDataProvider : public DataProvider<Scalar,Rank,Sequential> {
	typedef DataProvider<Scalar,Rank,Sequential> Base;
public:
	/**
	 * It constructs a data provider backed by the specified tensors.
	 *
	 * @param obs A unique pointer to the tensor containing the observations.
	 * @param obj A unique pointer to the tensor containing the objectives.
	 * @param shuffle Whether the 'rows' (first ranks) of the tensors should be randomly
	 * shuffled.
	 */
	inline MemoryDataProvider(typename Base::DataPtr obs, typename Base::DataPtr obj,
			bool shuffle = true) :
				obs(std::move(obs)),
				obj(std::move(obj)),
				offsets() {
		assert(this->obs != nullptr);
		assert(this->obj != nullptr);
		Utils<Scalar>::template check_dim_validity<Base::DATA_RANKS>(*this->obs);
		Utils<Scalar>::template check_dim_validity<Base::DATA_RANKS>(*this->obj);
		assert(this->obs->dimension(0) == this->obj->dimension(0) &&
				"mismatched data and obj tensor row numbers");
		Dimensions<std::size_t,Base::DATA_RANKS> obs_dims =
				Utils<Scalar>::template get_dims<Base::DATA_RANKS>(*this->obs);
		Dimensions<std::size_t,Base::DATA_RANKS> obj_dims =
				Utils<Scalar>::template get_dims<Base::DATA_RANKS>(*this->obj);
		this->obs_dims = obs_dims.template demote<Sequential + 1>();
		this->obj_dims = obj_dims.template demote<Sequential + 1>();
		instances = (std::size_t) this->obs->dimension(0);
		offsets.fill(0);
		data_extents = obs_dims;
		obj_extents = obj_dims;
		if (shuffle) {
			Utils<Scalar>::template shuffle_tensor_rows<Base::DATA_RANKS>(*this->obs);
			Utils<Scalar>::template shuffle_tensor_rows<Base::DATA_RANKS>(*this->obj);
		}
	}
	inline const Dimensions<std::size_t,Rank>& get_obs_dims() const {
		return obs_dims;
	}
	inline const Dimensions<std::size_t,Rank>& get_obj_dims() const {
		return obj_dims;
	}
	inline bool has_more() const {
		return offsets[0] < (int) instances;
	}
	inline DataPair<Scalar,Rank,Sequential> get_data(std::size_t batch_size) {
		std::size_t max_batch_size = std::min(batch_size, instances - offsets[0]);
		data_extents[0] = max_batch_size;
		obj_extents[0] = max_batch_size;
		typename Base::Data data_batch = obs->slice(offsets, data_extents);
		typename Base::Data obj_batch = obj->slice(offsets, obj_extents);
		offsets[0] = std::min(instances, offsets[0] + max_batch_size);
		return std::make_pair(data_batch, obj_batch);
	}
	inline void reset() {
		offsets[0] = 0;
	}
protected:
	inline void skip(std::size_t instances) {
		offsets[0] = std::min((int) this->instances, (int) (offsets[0] + instances));
	}
private:
	typename Base::DataPtr obs;
	typename Base::DataPtr obj;
	Dimensions<std::size_t,Rank> obs_dims;
	Dimensions<std::size_t,Rank> obj_dims;
	std::size_t instances;
	std::array<std::size_t,Base::DATA_RANKS> offsets;
	std::array<std::size_t,Base::DATA_RANKS> data_extents;
	std::array<std::size_t,Base::DATA_RANKS> obj_extents;
};

/**
 * An abstract class template for a data provider backed by data on disk in the form of an arbitrary
 * number of files containing both the observations and the objectives. Implementations are responsible
 * for specifying the dimensions of both the observations and the objectives, for reading batches of
 * observation-objective pairs from the file, and for skipping arbitrary number of data instances.
 */
template<typename Scalar, std::size_t Rank, bool Sequential>
class JointFileDataProvider : public DataProvider<Scalar,Rank,Sequential> {
	typedef DataProvider<Scalar,Rank,Sequential> Base;
public:
	virtual ~JointFileDataProvider() = default;
	inline bool has_more() const {
		return current_stream < data_streams.size() - 1 || (current_stream < data_streams.size() &&
				!data_streams[current_stream].eof());
	}
	inline DataPair<Scalar,Rank,Sequential> get_data(std::size_t batch_size) {
		if (!has_more())
			return std::make_pair(typename Base::Data(), typename Base::Data());
		DataPair<Scalar,Rank,Sequential> data_pair = _get_data(data_streams[current_stream],
				batch_size);
		assert(data_pair.first.dimension(0) == data_pair.second.dimension(0));
		while (data_pair.first.dimension(0) != batch_size && ++current_stream < data_streams.size()) {
			DataPair<Scalar,Rank,Sequential> additional_data_pair = _get_data(data_streams[current_stream],
					batch_size - data_pair.first.dimension(0));
			assert(additional_data_pair.first.dimension(0) == additional_data_pair.second.dimension(0));
			data_pair.first = data_pair.first.concatenate(additional_data_pair.first, 0);
			data_pair.second = data_pair.second.concatenate(additional_data_pair.second, 0);
		}
		return data_pair;
	}
	inline void reset() {
		for (std::size_t i = 0; i <= current_stream && i <= data_streams.size(); ++i)
			data_streams[i].seekg(0, std::ios::beg);
		current_stream = 0;
	}
protected:
	inline JointFileDataProvider(std::initializer_list<std::string> dataset_paths, bool binary) :
				data_streams(dataset_paths.size()),
				current_stream(0) {
		assert(dataset_paths.size() > 0);
		std::size_t i = 0;
		for (std::initializer_list<std::string>::iterator it = dataset_paths.begin();
				it != dataset_paths.end(); ++it) {
			data_streams[i] = std::ifstream(*it, binary ? std::ios::binary : std::ios::in);
			assert(data_streams[i++].is_open());
		}
	}
	inline JointFileDataProvider(std::vector<std::string> dataset_paths, bool binary) :
			data_streams(dataset_paths.size()),
			current_stream(0) {
		assert(!dataset_paths.empty());
		for (std::size_t i = 0; i < dataset_paths.size(); ++i) {
			data_streams[i] = std::ifstream(dataset_paths[i], binary ? std::ios::binary : std::ios::in);
			assert(data_streams[i].is_open());
		}
	}
	/**
	 * It reads at most the specified number of observation-objective pairs from the provided
	 * file stream.
	 *
	 * @param data_stream A reference to the file stream of the data set.
	 * @param batch_size The number of data points to return.
	 * @return A pair of tensors containing the data batch.
	 */
	virtual DataPair<Scalar,Rank,Sequential> _get_data(std::ifstream& data_stream,
			std::size_t batch_size) = 0;
	/**
	 * Skips at most the specified number of instances in the data stream.
	 *
	 * @param data_stream A reference to the file stream of the data set.
	 * @param instances The number of instances to skip.
	 * @return The number of instances actually skipped. It may be less than the specified
	 * amount if there are fewer remaining instances in the data stream.
	 */
	virtual std::size_t _skip(std::ifstream& data_stream, std::size_t instances) = 0;
	inline void skip(std::size_t instances) {
		if (!has_more())
			return;
		std::size_t skipped = _skip(data_streams[current_stream], instances);
		while (skipped != instances && ++current_stream < data_streams.size())
			skipped += _skip(data_streams[current_stream], instances - skipped);
	}
private:
	std::vector<std::ifstream> data_streams;
	std::size_t current_stream;
};

/**
 * An abstract class template for a data provider backed by an arbitrary number of file pairs
 * containing the separated observations and the objectives. Implementations are responsible for
 * specifying the dimensions of both the observations and the objectives, for reading batches of
 * observation-objective pairs from the file, and for skipping arbitrary number of data instances.
 */
template<typename Scalar, std::size_t Rank, bool Sequential>
class SplitFileDataProvider : public DataProvider<Scalar,Rank,Sequential> {
	typedef DataProvider<Scalar,Rank,Sequential> Base;
public:
	virtual ~SplitFileDataProvider() = default;
	inline bool has_more() const {
		return current_stream_pair < data_stream_pairs.size() - 1 ||
				(current_stream_pair < data_stream_pairs.size() &&
				!data_stream_pairs[current_stream_pair].first.eof() &&
				!data_stream_pairs[current_stream_pair].second.eof());
	}
	inline DataPair<Scalar,Rank,Sequential> get_data(std::size_t batch_size) {
		if (!has_more())
			return std::make_pair(typename Base::Data(), typename Base::Data());
		std::pair<std::ifstream,std::ifstream>& first_stream_pair =
				data_stream_pairs[current_stream_pair];
		DataPair<Scalar,Rank,Sequential> data_pair = _get_data(first_stream_pair.first,
				first_stream_pair.second, batch_size);
		assert(data_pair.first.dimension(0) == data_pair.second.dimension(0));
		while (data_pair.first.dimension(0) != batch_size &&
				++current_stream_pair < data_stream_pairs.size()) {
			std::pair<std::ifstream,std::ifstream>& stream_pair = data_stream_pairs[current_stream_pair];
			DataPair<Scalar,Rank,Sequential> additional_data_pair = _get_data(stream_pair.first,
					stream_pair.second, batch_size - data_pair.first.dimension(0));
			assert(additional_data_pair.first.dimension(0) == additional_data_pair.second.dimension(0));
			data_pair.first = data_pair.first.concatenate(additional_data_pair.first, 0);
			data_pair.second = data_pair.second.concatenate(additional_data_pair.second, 0);
		}
		return data_pair;
	}
	inline void reset() {
		for (std::size_t i = 0; i <= current_stream_pair && i <= data_stream_pairs.size(); ++i) {
			std::pair<std::ifstream,std::ifstream>& stream_pair = data_stream_pairs[i];
			stream_pair.first.seekg(0, std::ios::beg);
			stream_pair.second.seekg(0, std::ios::beg);
		}
		current_stream_pair = 0;
	}
protected:
	inline SplitFileDataProvider(std::initializer_list<std::pair<std::string,std::string>> obs_obj_pairs,
			bool obs_binary, bool obj_binary) :
				data_stream_pairs(obs_obj_pairs.size()),
				current_stream_pair(0) {
		assert(obs_obj_pairs.size() > 0);
		std::size_t i = 0;
		for (std::initializer_list<std::pair<std::string,std::string>>::iterator it = obs_obj_pairs.begin();
				it != obs_obj_pairs.end(); ++it) {
			std::pair<std::string,std::string>& path_pair = *it;
			std::ifstream obs_stream(path_pair.first, obs_binary ? std::ios::binary : std::ios::in);
			assert(obs_stream.is_open());
			std::ifstream obj_stream(path_pair.second, obj_binary ? std::ios::binary : std::ios::in);
			assert(obj_stream.is_open());
			data_stream_pairs[i++] = std::make_pair(obs_stream, obj_stream);
		}
	}
	inline SplitFileDataProvider(std::vector<std::pair<std::string,std::string>> obs_obj_pairs, bool obs_binary,
			bool obj_binary) :
				data_stream_pairs(obs_obj_pairs.size()),
				current_stream_pair(0) {
		assert(!obs_obj_pairs.empty());
		for (std::size_t i = 0; i < obs_obj_pairs.size(); ++i) {
			std::pair<std::string,std::string>& path_pair = obs_obj_pairs[i];
			std::ifstream obs_stream(path_pair.first, obs_binary ? std::ios::binary : std::ios::in);
			assert(obs_stream.is_open());
			std::ifstream obj_stream(path_pair.second, obj_binary ? std::ios::binary : std::ios::in);
			assert(obj_stream.is_open());
			data_stream_pairs[i] = std::make_pair(obs_stream, obj_stream);
		}
	}
	/**
	 * It reads at most the specified number of observations from the observation-file and
	 * at most the specified number of objectives from the objective-file.
	 *
	 * @param obs_input_stream A reference to the file stream to a file containing
	 * observations.
	 * @param obj_input_stream A reference to the file stream to a file containing
	 * objectives.
	 * @param batch_size The number of data points to read.
	 * @return The paired observations and objectives.
	 */
	virtual DataPair<Scalar,Rank,Sequential> _get_data(std::ifstream& obs_input_stream,
			std::ifstream& obj_input_stream, std::size_t batch_size) = 0;
	/**
	 * Skips at most the specified number of instances in the data streams.
	 *
	 * @param obs_input_stream A reference to the file stream to a file containing
	 * observations.
	 * @param obj_input_stream A reference to the file stream to a file containing
	 * objectives.
	 * @param instances The number of data points to skip.
	 * @return The number of actual data points skipped. It may be less than the specified
	 * amount if there are fewer remaining instances in the data streams.
	 */
	virtual std::size_t _skip(std::ifstream& obs_input_stream, std::ifstream& obj_input_stream,
			std::size_t instances) = 0;
	inline void skip(std::size_t instances) {
		if (!has_more())
			return;
		std::pair<std::ifstream,std::ifstream>& first_stream_pair = data_stream_pairs[current_stream_pair];
		std::size_t skipped = _skip(first_stream_pair.first, first_stream_pair.second, instances);
		while (skipped != instances && ++current_stream_pair < data_stream_pairs.size()) {
			std::pair<std::ifstream,std::ifstream>& stream_pair = data_stream_pairs[current_stream_pair];
			skipped += _skip(stream_pair.first, stream_pair.second, instances - skipped);
		}
	}
private:
	std::vector<std::pair<std::ifstream,std::ifstream>> data_stream_pairs;
	std::size_t current_stream_pair;
};

/**
 * A data provider template for the CIFAR-10 data set.
 */
template<typename Scalar>
class CIFAR10DataProvider : public JointFileDataProvider<Scalar,3,false> {
	typedef DataProvider<Scalar,3,false> Root;
	typedef JointFileDataProvider<Scalar,3,false> Base;
	static constexpr std::size_t INSTANCE_LENGTH = 3073;
public:
	inline CIFAR10DataProvider(std::initializer_list<std::string> files) :
			Base::JointFileDataProvider(files, true),
			obs({ 32u, 32u, 3u }),
			obj({ 1u, 1u, 1u }) { }
	inline const Dimensions<std::size_t,3>& get_obs_dims() const {
		return obs;
	}
	inline const Dimensions<std::size_t,3>& get_obj_dims() const {
		return obj;
	}
protected:
	inline DataPair<Scalar,3,false> _get_data(std::ifstream& data_stream,
				std::size_t batch_size) {
		Tensor<Scalar,4> obs(batch_size, 32u, 32u, 3u);
		Tensor<Scalar,4> obj(batch_size, 1u, 1u, 1u);
		std::size_t i;
		for (i = 0; !data_stream.eof(); ++i) {
			data_stream.read(buffer, INSTANCE_LENGTH);
			obj(i,0,0,0) = (Scalar) buffer[0];
			std::size_t buffer_ind = 1;
			for (std::size_t channel = 0; channel < 3; ++channel) {
				for (std::size_t row = 0; row < 32; ++row) {
					for (std::size_t height = 0; height < 32; ++height)
						obs(i,height,row,channel) = (Scalar) buffer[buffer_ind++];
				}
			}
			assert(buffer_ind == INSTANCE_LENGTH);
		}
		if (i == batch_size)
			return std::make_pair(obs, obj);
		std::array<std::size_t,4> offsets({ 0, 0, 0, 0 });
		std::array<std::size_t,4> obs_extents({ i, 32u, 32u, 3u });
		std::array<std::size_t,4> obj_extents({ i, 1u, 1u, 1u });
		Tensor<Scalar,4> obs_slice = obs.slice(offsets, obs_extents);
		Tensor<Scalar,4> obj_slice = obj.slice(offsets, obs_extents);
		return std::make_pair(obs_slice, obj_slice);
	}
	inline std::size_t _skip(std::ifstream& data_stream, std::size_t instances) {
		data_stream.seekg(data_stream.tellg() + instances * INSTANCE_LENGTH);
	}
private:
	const Dimensions<std::size_t,3> obs;
	const Dimensions<std::size_t,3> obj;
	char buffer[INSTANCE_LENGTH];
};

} /* namespace cattle */

#endif /* DATAPROVIDER_H_ */
