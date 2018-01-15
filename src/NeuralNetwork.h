/*
 * NeuralNetwork.h
 *
 *  Created on: 04.12.2017
 *      Author: Viktor Csomor
 */

#ifndef NEURALNETWORK_H_
#define NEURALNETWORK_H_

#include <Layer.h>
#include <cassert>
#include <Dimensions.h>
#include <iomanip>
#include <Matrix.h>
#include <sstream>
#include <string>
#include <Tensor.h>
#include <vector>
#include <Vector.h>
#include <WeightInitialization.h>

namespace cppnn {

// Forward declaration to Optimizer so it can be friended.
template<typename Scalar>
class Optimizer;

/**
 * A neural network class that consists of a vector of neuron layers. It allows
 * for the calculation of the output of the network given an input vector and
 * it provides a member function for back-propagating the derivative of a loss
 * function with respect to the activated output of the network's last layer.
 * This back-propagation sets the gradients of the parameters associated with each
 * layer.
 */
template<typename Scalar>
class NeuralNetwork {
	friend class Optimizer<Scalar>;
public:
	virtual ~NeuralNetwork() = default;
	virtual Dimensions get_input_dims() const = 0;
	virtual Dimensions get_output_dims() const = 0;
	virtual void init() {
		std::vector<Layer<Scalar>*> layers = get_layers();
		for (unsigned i = 0; i < layers.size(); i++)
			layers[i]->init();
	};
	virtual Tensor4D<Scalar> infer(Tensor4D<Scalar> input) {
		return propagate(input, false);
	};
	virtual std::string to_string() {
		std::stringstream strm;
		strm << "Neural Net " << this << std::endl;
		for (unsigned i = 0; i < get_layers().size(); i++) {
			Layer<Scalar>* layer = get_layers()[i];
			strm << "\tLayer " << std::setw(3) << std::to_string(i + 1) <<
					"----------------------------" << std::endl;
			strm << "\t\tinput dims: " << layer->get_input_dims().to_string() << std::endl;
			strm << "\t\toutput dims: " << layer->get_output_dims().to_string() << std::endl;
			if (layer->is_parametric()) {
				strm << "\t\tparams:" << std::endl;
				Matrix<Scalar>& params = layer->get_params();
				for (int j = 0; j < params.rows(); j++) {
					strm << "\t\t[ ";
					for (int k = 0; k < params.cols(); k++) {
						strm << std::setw(11) << std::setprecision(4) << params(j,k);
						if (k != params.cols() - 1) {
							strm << ", ";
						}
					}
					strm << " ]" << std::endl;
				}
			}
		}
		return strm.str();
	};
protected:
	virtual std::vector<Layer<Scalar>*>& get_layers() = 0;
	virtual Tensor4D<Scalar> propagate(Tensor4D<Scalar> input, bool training) = 0;
	virtual void backpropagate(Tensor4D<Scalar> out_grads) = 0;
	static Tensor4D<Scalar> pass_forward(Layer<Scalar>& layer, Tensor4D<Scalar> prev_out, bool training) {
		return layer.pass_forward(prev_out, training);
	};
	static Tensor4D<Scalar> pass_back(Layer<Scalar>& layer, Tensor4D<Scalar> out_grads) {
		return layer.pass_back(out_grads);
	};
};

template<typename Scalar>
class FFNeuralNetwork : public NeuralNetwork<Scalar> {
public:
	/**
	 * Constructs the network using the provided vector of layers. The object
	 * takes ownership of the pointers held in the vector and deletes them when
	 * its destructor is invoked.
	 *
	 * @param layers A vector of Layer pointers to compose the neural network.
	 * The vector must contain at least 1 element and no null pointers. The
	 * the prev_nodes attributes of the Layer objects pointed to by the pointers
	 * must match the nodes attribute of the Layer objects pointed to by their
	 * pointers directly preceding them in the vector.
	 */
	FFNeuralNetwork(std::vector<Layer<Scalar>*> layers) : // TODO use smart Layer pointers
			layers(layers) {
		assert(layers.size() > 0 && "layers must contain at least 1 element");
		input_dims = layers[0]->get_input_dims();
		output_dims = layers[layers.size() - 1]->get_output_dims();
		Dimensions prev_dims = layers[0]->get_output_dims();
		for (unsigned i = 1; i < layers.size(); i++) {
			assert(layers[i] != nullptr && "layers contains null pointers");
			assert(prev_dims.equals(layers[i]->get_input_dims()) && "incompatible layer dimensions");
			prev_dims = layers[i]->get_output_dims();
		}
	};
	// Copy constructor.
	FFNeuralNetwork(const FFNeuralNetwork<Scalar>& network) :
			layers(network.layers.size()) {
		for (unsigned i = 0; i < layers.size(); i++) {
			layers[i] = network.layers[i]->clone();
		}
		input_dims = network.input_dims;
		output_dims = network.output_dims;
	};
	// Move constructor.
	FFNeuralNetwork(FFNeuralNetwork<Scalar>&& network) {
		swap(*this, network);
	};
	// Take ownership of layer pointers.
	~FFNeuralNetwork() {
		for (unsigned i = 0; i < layers.size(); i++) {
			delete layers[i];
		}
	};
	/* The assignment uses the move or copy constructor to pass the parameter
	 * based on whether it is an lvalue or an rvalue. */
	FFNeuralNetwork<Scalar>& operator=(FFNeuralNetwork<Scalar> network) {
		swap(*this, network);
		return *this;
	};
	Dimensions get_input_dims() const {
		return input_dims;
	};
	Dimensions get_output_dims() const {
		return output_dims;
	};
protected:
	// For the copy-and-swap idiom.
	friend void swap(FFNeuralNetwork<Scalar>& network1,
			FFNeuralNetwork<Scalar>& network2) {
		using std::swap;
		swap(network1.layers, network2.layers);
		swap(network1.input_dims, network2.input_dims);
		swap(network1.output_dims, network2.output_dims);
	};
	std::vector<Layer<Scalar>*>& get_layers() {
		return layers;
	};
	Tensor4D<Scalar> propagate(Tensor4D<Scalar> input, bool training) {
		assert(input.dimension(0) && "empty neural network input");
		assert(input.dimension(1) == input_dims.get_dim1() && input.dimension(2) == input_dims.get_dim2() &&
				input.dimension(3) == input_dims.get_dim3() && "wrong neural network input size");
		for (unsigned i = 0; i < layers.size(); i++)
			input = NeuralNetwork<Scalar>::pass_forward(*(layers[i]), input, training);
		return input;
	};
	void backpropagate(Tensor4D<Scalar> out_grads) {
		assert(out_grads.dimension(0) && "empty neural network output gradient");
		assert(out_grads.dimension(1) == output_dims.get_dim1() && out_grads.dimension(2) == output_dims.get_dim2() &&
				out_grads.dimension(3) == output_dims.get_dim3() && "wrong neural network output gradient size");
		for (int i = layers.size() - 1; i >= 0; i--)
			out_grads = NeuralNetwork<Scalar>::pass_back(*(layers[i]), out_grads);
	};
	std::vector<Layer<Scalar>*> layers;
	Dimensions input_dims;
	Dimensions output_dims;
};

template<typename Scalar>
class ResNeuralNetwork : public FFNeuralNetwork<Scalar> {
public:
		ResNeuralNetwork(std::vector<Layer<Scalar>*> layers) :
				FFNeuralNetwork<Scalar>::FFNeuralNetwork(layers) { };
		ResNeuralNetwork(const ResNeuralNetwork<Scalar>& network) :
				FFNeuralNetwork<Scalar>::FFNeuralNetwork(network) { };
		ResNeuralNetwork(ResNeuralNetwork<Scalar>&& network) :
				FFNeuralNetwork<Scalar>::FFNeuralNetwork(network) { };
		ResNeuralNetwork<Scalar>& operator=(ResNeuralNetwork<Scalar> network) {
			FFNeuralNetwork<Scalar>::swap(*this, network);
			return *this;
		};
protected:
		Matrix<Scalar> propagate(Tensor4D<Scalar> input, bool training) {
			assert(input.dimension(0) && "empty neural network input");
			assert(input.dimension(1) == FFNeuralNetwork<Scalar>::input_dims.get_dim1() &&
					input.dimension(2) == FFNeuralNetwork<Scalar>::input_dims.get_dim2() &&
					input.dimension(3) == FFNeuralNetwork<Scalar>::input_dims.get_dim3() &&
					"wrong neural network input size");
			for (unsigned i = 0; i < FFNeuralNetwork<Scalar>::layers.size(); i++)
				input = NeuralNetwork<Scalar>::pass_forward(*(FFNeuralNetwork<Scalar>::layers[i]), input, training);
			return input;
		};
		void backpropagate(Tensor4D<Scalar> out_grads) {
			assert(out_grads.dimension(0) && "empty neural network output gradient");
			assert(out_grads.dimension(1) == FFNeuralNetwork<Scalar>::output_dims.get_dim1() &&
					out_grads.dimension(2) == FFNeuralNetwork<Scalar>::output_dims.get_dim2() &&
					out_grads.dimension(3) == FFNeuralNetwork<Scalar>::output_dims.get_dim3() &&
					"wrong neural network output gradient size");
			for (int i = FFNeuralNetwork<Scalar>::layers.size() - 1; i >= 0; i--)
				out_grads = NeuralNetwork<Scalar>::pass_back(*(FFNeuralNetwork<Scalar>::layers[i]), out_grads);
		};
};

} /* namespace cppnn */

#endif /* NEURALNETWORK_H_ */
