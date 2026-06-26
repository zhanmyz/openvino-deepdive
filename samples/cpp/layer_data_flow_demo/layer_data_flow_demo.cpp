#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// 这份 demo 参考的是 OpenVINO 的“图描述”和“运行时张量映射”两层设计：
// 1. src/core/include/openvino/core/node_output.hpp
// 2. src/core/include/openvino/core/node_input.hpp
// 3. src/core/include/openvino/core/descriptor/tensor.hpp
// 4. src/inference/src/dev/isync_infer_request.cpp
// 5. src/inference/src/model_reader.cpp
//
// 重点结论：
// - 现代 OpenVINO 的图层连接，核心不是老的 Blob 概念。
// - 图上连接的是 Output/Input 端口，以及它们共享的 descriptor::Tensor。
// - 到真正推理时，再把 descriptor::Tensor 映射到具体的内存对象 ITensor。
//
// 这个 demo 故意不用 OpenVINO API，而是用纯 C++ 自己实现一套“迷你版”模型，
// 帮助初学者看懂“上一层输出，下一层如何接到这份数据”。

struct PortInfo {
    int id = -1;
    std::string precision;
    std::string names;
    std::vector<size_t> dims;
};

struct LayerInfo {
    int id = -1;
    std::string name;
    std::string type;
    std::vector<PortInfo> inputs;
    std::vector<PortInfo> outputs;
};

struct EdgeInfo {
    int from_layer = -1;
    int from_port = -1;
    int to_layer = -1;
    int to_port = -1;
};

// 这就是本 demo 里的“数据块”。
// 你可以把它理解成“运行时真正承载数值的缓冲区”。
// 为了直观，我们只放 shape、少量 preview 数值和生产者信息。
// 真正的 OpenVINO 里，图结构层面对应 descriptor::Tensor，
// 推理请求层面对应 ov::ITensor / ov::Tensor。
struct TensorBuffer {
    std::string logical_name;
    std::vector<size_t> shape;
    std::vector<float> preview_values;
    std::string producer_layer;
};

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string get_attribute(const std::string& line, const std::string& key) {
    const std::regex pattern(key + "=\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(line, match, pattern)) {
        return match[1].str();
    }
    return "";
}

std::optional<size_t> parse_dim_line(const std::string& line) {
    const std::regex pattern(R"(<dim>([0-9]+)</dim>)");
    std::smatch match;
    if (!std::regex_search(line, match, pattern)) {
        return std::nullopt;
    }
    return static_cast<size_t>(std::stoull(match[1].str()));
}

std::string port_key(int layer_id, int port_id) {
    return std::to_string(layer_id) + ":" + std::to_string(port_id);
}

std::string shape_to_string(const std::vector<size_t>& dims) {
    if (dims.empty()) {
        return "[]";
    }

    std::ostringstream stream;
    stream << "[";
    for (size_t index = 0; index < dims.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        stream << dims[index];
    }
    stream << "]";
    return stream.str();
}

std::string preview_to_string(const std::vector<float>& values) {
    std::ostringstream stream;
    stream << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        stream << std::fixed << std::setprecision(2) << values[index];
    }
    stream << "]";
    return stream.str();
}

size_t element_count(const std::vector<size_t>& dims) {
    if (dims.empty()) {
        return 0;
    }

    return std::accumulate(dims.begin(), dims.end(), static_cast<size_t>(1), std::multiplies<size_t>());
}

const PortInfo* find_output_port(const LayerInfo& layer, int port_id) {
    for (const auto& port : layer.outputs) {
        if (port.id == port_id) {
            return &port;
        }
    }
    return nullptr;
}

const PortInfo* find_input_port(const LayerInfo& layer, int port_id) {
    for (const auto& port : layer.inputs) {
        if (port.id == port_id) {
            return &port;
        }
    }
    return nullptr;
}

class IRXmlParser {
public:
    explicit IRXmlParser(std::filesystem::path xml_path) : m_xml_path(std::move(xml_path)) {}

    void parse() {
        std::ifstream input(m_xml_path);
        if (!input) {
            throw std::runtime_error("Cannot open XML file: " + m_xml_path.string());
        }

        bool inside_layers = false;
        bool inside_edges = false;
        bool inside_layer = false;
        bool inside_input = false;
        bool inside_output = false;
        bool inside_port = false;

        LayerInfo current_layer;
        PortInfo current_port;

        std::string line;
        while (std::getline(input, line)) {
            const std::string text = trim(line);
            if (text.empty()) {
                continue;
            }

            if (text == "<layers>") {
                inside_layers = true;
                continue;
            }
            if (text == "</layers>") {
                inside_layers = false;
                continue;
            }
            if (text == "<edges>") {
                inside_edges = true;
                continue;
            }
            if (text == "</edges>") {
                inside_edges = false;
                continue;
            }

            if (inside_layers) {
                if (text.rfind("<layer ", 0) == 0) {
                    inside_layer = true;
                    current_layer = {};
                    current_layer.id = std::stoi(get_attribute(text, "id"));
                    current_layer.name = get_attribute(text, "name");
                    current_layer.type = get_attribute(text, "type");
                    continue;
                }

                if (!inside_layer) {
                    continue;
                }

                if (text == "<input>") {
                    inside_input = true;
                    continue;
                }
                if (text == "</input>") {
                    inside_input = false;
                    continue;
                }
                if (text == "<output>") {
                    inside_output = true;
                    continue;
                }
                if (text == "</output>") {
                    inside_output = false;
                    continue;
                }

                if (text.rfind("<port ", 0) == 0) {
                    inside_port = true;
                    current_port = {};
                    current_port.id = std::stoi(get_attribute(text, "id"));
                    current_port.precision = get_attribute(text, "precision");
                    current_port.names = get_attribute(text, "names");
                    continue;
                }

                if (inside_port) {
                    if (const auto dim = parse_dim_line(text)) {
                        current_port.dims.push_back(*dim);
                        continue;
                    }

                    if (text == "</port>") {
                        inside_port = false;
                        if (inside_input) {
                            current_layer.inputs.push_back(current_port);
                        } else if (inside_output) {
                            current_layer.outputs.push_back(current_port);
                        }
                        continue;
                    }
                }

                if (text == "</layer>") {
                    inside_layer = false;
                    m_layers.push_back(current_layer);
                    continue;
                }
            }

            if (inside_edges && text.rfind("<edge ", 0) == 0) {
                EdgeInfo edge;
                edge.from_layer = std::stoi(get_attribute(text, "from-layer"));
                edge.from_port = std::stoi(get_attribute(text, "from-port"));
                edge.to_layer = std::stoi(get_attribute(text, "to-layer"));
                edge.to_port = std::stoi(get_attribute(text, "to-port"));
                m_edges.push_back(edge);
            }
        }

        std::sort(m_layers.begin(), m_layers.end(), [](const LayerInfo& lhs, const LayerInfo& rhs) {
            return lhs.id < rhs.id;
        });
    }

    const std::vector<LayerInfo>& layers() const {
        return m_layers;
    }

    const std::vector<EdgeInfo>& edges() const {
        return m_edges;
    }

private:
    std::filesystem::path m_xml_path;
    std::vector<LayerInfo> m_layers;
    std::vector<EdgeInfo> m_edges;
};

class MiniGraphRuntime {
public:
    MiniGraphRuntime(std::vector<LayerInfo> layers, std::vector<EdgeInfo> edges)
        : m_layers(std::move(layers)), m_edges(std::move(edges)) {
        for (const auto& layer : m_layers) {
            m_layers_by_id.emplace(layer.id, layer);
        }

        for (const auto& edge : m_edges) {
            m_incoming_edges[edge.to_layer].push_back(edge);
            m_outgoing_edges[port_key(edge.from_layer, edge.from_port)].push_back(edge);
        }

        for (auto& [layer_id, edges_for_layer] : m_incoming_edges) {
            std::sort(edges_for_layer.begin(), edges_for_layer.end(), [](const EdgeInfo& lhs, const EdgeInfo& rhs) {
                return lhs.to_port < rhs.to_port;
            });
        }
    }

    void print_summary() const {
        std::cout << "================ Graph Summary ================\n";
        std::cout << "Layer count : " << m_layers.size() << "\n";
        std::cout << "Edge count  : " << m_edges.size() << "\n";

        const auto parameter_count = std::count_if(m_layers.begin(), m_layers.end(), [](const LayerInfo& layer) {
            return layer.type == "Parameter";
        });
        const auto const_count = std::count_if(m_layers.begin(), m_layers.end(), [](const LayerInfo& layer) {
            return layer.type == "Const";
        });

        std::cout << "Parameters  : " << parameter_count << "\n";
        std::cout << "Consts      : " << const_count << "\n";

        for (const auto& [source_key, consumers] : m_outgoing_edges) {
            if (consumers.size() <= 1) {
                continue;
            }

            const auto colon = source_key.find(':');
            const int layer_id = std::stoi(source_key.substr(0, colon));
            const int port_id = std::stoi(source_key.substr(colon + 1));
            const auto layer_it = m_layers_by_id.find(layer_id);
            if (layer_it == m_layers_by_id.end()) {
                continue;
            }

            std::cout << "Fan-out example: layer " << layer_id << " (" << layer_it->second.name << ")"
                      << " output port " << port_id << " has " << consumers.size()
                      << " downstream consumers.\n";
            break;
        }

        std::cout << '\n';
    }

    void seed_source_layers(size_t max_compute_layers) {
        std::cout << "================ Seed Source Tensors ================\n";
        const auto relevant_sources = collect_relevant_sources(max_compute_layers);
        size_t printed = 0;
        size_t hidden = 0;

        for (const auto& layer : m_layers) {
            if (layer.type != "Parameter" && layer.type != "Const") {
                continue;
            }

            for (const auto& output : layer.outputs) {
                auto buffer = make_buffer(layer, output, {});
                const auto key = port_key(layer.id, output.id);
                m_buffers[key] = buffer;

                if (!relevant_sources.count(key)) {
                    ++hidden;
                    continue;
                }

                std::cout << "Seed layer [" << layer.id << "] " << layer.type << " -> tensor "
                          << buffer->logical_name << " @" << buffer.get() << " shape="
                          << shape_to_string(buffer->shape) << " preview="
                          << preview_to_string(buffer->preview_values) << '\n';
                ++printed;
            }
        }

        std::cout << "Printed source tensors: " << printed << "\n";
        std::cout << "Hidden source tensors : " << hidden
                  << " (they exist in the full model, but are not needed for this short demo)\n";
        std::cout << '\n';
    }

    void execute_first_layers(size_t max_compute_layers) {
        std::cout << "================ Simulated Execution ================\n";
        std::cout << "Important: below is not real convolution math.\n";
        std::cout << "It only simulates how one layer output becomes the next layer input.\n\n";

        size_t executed = 0;
        for (const auto& layer : m_layers) {
            if (layer.type == "Parameter" || layer.type == "Const") {
                continue;
            }

            const auto incoming_it = m_incoming_edges.find(layer.id);
            if (incoming_it == m_incoming_edges.end()) {
                continue;
            }

            std::vector<std::shared_ptr<TensorBuffer>> input_buffers;
            bool ready = true;
            for (const auto& edge : incoming_it->second) {
                const auto source_it = m_buffers.find(port_key(edge.from_layer, edge.from_port));
                if (source_it == m_buffers.end()) {
                    ready = false;
                    break;
                }
                input_buffers.push_back(source_it->second);
            }

            if (!ready) {
                continue;
            }

            std::cout << "Layer [" << layer.id << "] " << layer.name << " (" << layer.type << ")\n";
            for (const auto& edge : incoming_it->second) {
                const auto& source_buffer = m_buffers.at(port_key(edge.from_layer, edge.from_port));
                const auto source_layer_it = m_layers_by_id.find(edge.from_layer);
                const std::string source_name =
                    source_layer_it == m_layers_by_id.end() ? "<unknown>" : source_layer_it->second.name;

                std::cout << "  Input port " << edge.to_port << " <- layer " << edge.from_layer << ":port "
                          << edge.from_port << " (" << source_name << ") uses tensor "
                          << source_buffer->logical_name << " @" << source_buffer.get() << " shape="
                          << shape_to_string(source_buffer->shape) << '\n';
            }

            for (const auto& output : layer.outputs) {
                auto buffer = make_buffer(layer, output, input_buffers);
                m_buffers[port_key(layer.id, output.id)] = buffer;

                std::cout << "  Output port " << output.id << " -> create tensor "
                          << buffer->logical_name << " @" << buffer.get() << " shape="
                          << shape_to_string(buffer->shape) << " preview="
                          << preview_to_string(buffer->preview_values) << '\n';

                const auto outgoing_it = m_outgoing_edges.find(port_key(layer.id, output.id));
                if (outgoing_it != m_outgoing_edges.end() && outgoing_it->second.size() > 1) {
                    std::cout << "    This output fans out to " << outgoing_it->second.size()
                              << " downstream inputs. In OpenVINO terms, several Input ports can point to\n"
                              << "    the same logical tensor edge before runtime binds concrete memory.\n";
                }
            }

            std::cout << '\n';
            ++executed;
            if (executed >= max_compute_layers) {
                break;
            }
        }
    }

    void print_conclusion() const {
        std::cout << "================ Conclusion ================\n";
        std::cout << "1. Layer and layer are connected by edges.\n";
        std::cout << "2. Each edge represents one logical tensor flowing out of one layer port and into another layer port.\n";
        std::cout << "3. In old Inference Engine material you may see Blob. That is legacy terminology.\n";
        std::cout << "4. Modern OpenVINO graph code mainly talks about Output/Input + descriptor::Tensor.\n";
        std::cout << "5. At runtime, infer request code maps that logical tensor to a real memory object (ITensor).\n";
    }

private:
    std::unordered_set<std::string> collect_relevant_sources(size_t max_compute_layers) const {
        std::unordered_set<std::string> relevant_sources;
        size_t selected_compute_layers = 0;

        for (const auto& layer : m_layers) {
            if (layer.type == "Parameter" || layer.type == "Const") {
                continue;
            }

            const auto incoming_it = m_incoming_edges.find(layer.id);
            if (incoming_it == m_incoming_edges.end()) {
                continue;
            }

            for (const auto& edge : incoming_it->second) {
                relevant_sources.insert(port_key(edge.from_layer, edge.from_port));
            }

            ++selected_compute_layers;
            if (selected_compute_layers >= max_compute_layers) {
                break;
            }
        }

        return relevant_sources;
    }

    std::shared_ptr<TensorBuffer> make_buffer(const LayerInfo& layer,
                                              const PortInfo& output,
                                              const std::vector<std::shared_ptr<TensorBuffer>>& inputs) {
        auto buffer = std::make_shared<TensorBuffer>();
        buffer->logical_name = "tensor#" + std::to_string(m_next_tensor_id++);
        buffer->shape = !output.dims.empty() ? output.dims : infer_shape_from_inputs(inputs);
        buffer->producer_layer = layer.name;

        const size_t preview_size = std::min<size_t>(6, std::max<size_t>(1, element_count(buffer->shape)));
        buffer->preview_values.reserve(preview_size);

        for (size_t index = 0; index < preview_size; ++index) {
            float value = static_cast<float>(layer.id) + static_cast<float>(output.id) * 0.1F + static_cast<float>(index);
            if (!inputs.empty() && index < inputs.front()->preview_values.size()) {
                value = inputs.front()->preview_values[index] + static_cast<float>(layer.id) * 0.01F;
            }
            buffer->preview_values.push_back(value);
        }

        return buffer;
    }

    std::vector<size_t> infer_shape_from_inputs(const std::vector<std::shared_ptr<TensorBuffer>>& inputs) const {
        if (!inputs.empty()) {
            return inputs.front()->shape;
        }
        return {};
    }

    std::vector<LayerInfo> m_layers;
    std::vector<EdgeInfo> m_edges;
    std::map<int, LayerInfo> m_layers_by_id;
    std::unordered_map<int, std::vector<EdgeInfo>> m_incoming_edges;
    std::unordered_map<std::string, std::vector<EdgeInfo>> m_outgoing_edges;
    std::unordered_map<std::string, std::shared_ptr<TensorBuffer>> m_buffers;
    size_t m_next_tensor_id = 1;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [model.xml] [max_compute_layers]\n";
    std::cout << "Example:\n";
    std::cout << "  " << program << " models/yolo/yolo26n_openvino_model/yolo26n.xml 6\n\n";
    std::cout << "If no arguments are provided, the demo uses:\n";
    std::cout << "  models/yolo/yolo26n_openvino_model/yolo26n.xml\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3) {
            print_usage(argv[0]);
            return 1;
        }

        std::filesystem::path xml_path = "models/yolo/yolo26n_openvino_model/yolo26n.xml";
        size_t max_compute_layers = 6;

        if (argc >= 2) {
            xml_path = argv[1];
        }
        if (argc == 3) {
            max_compute_layers = static_cast<size_t>(std::stoull(argv[2]));
        }

        std::cout << "OpenVINO IR layer data-flow demo (pure C++)\n";
        std::cout << "Model XML        : " << xml_path.string() << '\n';
        std::cout << "Compute layers    : " << max_compute_layers << "\n\n";

        IRXmlParser parser(xml_path);
        parser.parse();

        MiniGraphRuntime runtime(parser.layers(), parser.edges());
        runtime.print_summary();
        runtime.seed_source_layers(max_compute_layers);
        runtime.execute_first_layers(max_compute_layers);
        runtime.print_conclusion();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}