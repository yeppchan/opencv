// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2018 Intel Corporation


#include <functional>
#include <iostream>
#include <iomanip> // std::fixed, std::setprecision
#include <unordered_set>
#include <stack>

#include <ade/util/algorithm.hpp>
#include <ade/util/chain_range.hpp>
#include <ade/util/range.hpp>
#include <ade/util/zip_range.hpp>

#include <ade/typed_graph.hpp>
#include <ade/execution_engine/execution_engine.hpp>

#include "opencv2/gapi/gcommon.hpp"
#include "logger.hpp"

#include "opencv2/gapi/own/convert.hpp"
#include "opencv2/gapi/gmat.hpp"    //for version of descr_of
// PRIVATE STUFF!
#include "compiler/gobjref.hpp"
#include "compiler/gmodel.hpp"

#include "backends/fluid/gfluidbuffer_priv.hpp"
#include "backends/fluid/gfluidbackend.hpp"
#include "backends/fluid/gfluidimgproc.hpp"
#include "backends/fluid/gfluidcore.hpp"

#include "api/gbackend_priv.hpp" // FIXME: Make it part of Backend SDK!

// FIXME: Is there a way to take a typed graph (our GModel),
// and create a new typed graph _ATOP_ of that (by extending with a couple of
// new types?).
// Alternatively, is there a way to compose types graphs?
//
// If not, we need to introduce that!
using GFluidModel = ade::TypedGraph
    < cv::gimpl::FluidUnit
    , cv::gimpl::FluidData
    , cv::gimpl::Protocol
    , cv::gimpl::FluidUseOwnBorderBuffer
    >;

// FIXME: Same issue with Typed and ConstTyped
using GConstFluidModel = ade::ConstTypedGraph
    < cv::gimpl::FluidUnit
    , cv::gimpl::FluidData
    , cv::gimpl::Protocol
    , cv::gimpl::FluidUseOwnBorderBuffer
    >;

// FluidBackend middle-layer implementation ////////////////////////////////////
namespace
{
    class GFluidBackendImpl final: public cv::gapi::GBackend::Priv
    {
        virtual void unpackKernel(ade::Graph            &graph,
                                  const ade::NodeHandle &op_node,
                                  const cv::GKernelImpl &impl) override
        {
            GFluidModel fm(graph);
            auto fluid_impl = cv::util::any_cast<cv::GFluidKernel>(impl.opaque);
            fm.metadata(op_node).set(cv::gimpl::FluidUnit{fluid_impl, {}, 0, 0, 0.0});
        }

        virtual EPtr compile(const ade::Graph &graph,
                             const cv::GCompileArgs &args,
                             const std::vector<ade::NodeHandle> &nodes) const override
        {
            const auto out_rois = cv::gimpl::getCompileArg<cv::GFluidOutputRois>(args).value_or(cv::GFluidOutputRois());
            return EPtr{new cv::gimpl::GFluidExecutable(graph, nodes, out_rois.rois)};
        }

        virtual void addBackendPasses(ade::ExecutionEngineSetupContext &ectx) override;

    };
}

cv::gapi::GBackend cv::gapi::fluid::backend()
{
    static cv::gapi::GBackend this_backend(std::make_shared<GFluidBackendImpl>());
    return this_backend;
}

// FluidAgent implementation ///////////////////////////////////////////////////

namespace cv { namespace gimpl {
struct FluidFilterAgent : public FluidAgent
{
private:
    virtual int firstWindow() const override;
    virtual int nextWindow() const override;
    virtual int linesRead() const override;
public:
    using FluidAgent::FluidAgent;
};

struct FluidResizeAgent : public FluidAgent
{
private:
    virtual int firstWindow() const override;
    virtual int nextWindow() const override;
    virtual int linesRead() const override;
public:
    using FluidAgent::FluidAgent;
};

struct FluidUpscaleAgent : public FluidAgent
{
private:
    virtual int firstWindow() const override;
    virtual int nextWindow() const override;
    virtual int linesRead() const override;
public:
    using FluidAgent::FluidAgent;
};
}} // namespace cv::gimpl

cv::gimpl::FluidAgent::FluidAgent(const ade::Graph &g, ade::NodeHandle nh)
    : k(GConstFluidModel(g).metadata(nh).get<FluidUnit>().k)        // init(0)
    , op_handle(nh)                                                 // init(1)
    , op_name(GModel::ConstGraph(g).metadata(nh).get<Op>().k.name)  // init(2)
{
    std::set<int> out_w;
    std::set<int> out_h;
    GModel::ConstGraph cm(g);
    for (auto out_data : nh->outNodes())
    {
        const auto  &d      = cm.metadata(out_data).get<Data>();
        cv::GMatDesc d_meta = cv::util::get<cv::GMatDesc>(d.meta);
        out_w.insert(d_meta.size.width);
        out_h.insert(d_meta.size.height);
    }

    // Different output sizes are not supported
    GAPI_Assert(out_w.size() == 1 && out_h.size() == 1);
}

void cv::gimpl::FluidAgent::reset()
{
    m_producedLines = 0;

    auto lines = firstWindow();
    for (auto &v : in_views)
    {
        if (v)
        {
            v.priv().reset(lines);
        }
    }
}

namespace {
static int calcGcd (int n1, int n2)
{
    return (n2 == 0) ? n1 : calcGcd (n2, n1 % n2);
}

// This is an empiric formula and this is not 100% guaranteed
// that it produces correct results in all possible cases
// FIXME:
// prove correctness or switch to some trusted method
//
// When performing resize input/output pixels form a cyclic
// pattern where inH/gcd input pixels are mapped to outH/gcd
// output pixels (pattern repeats gcd times).
//
// Output pixel can partually cover some of the input pixels.
// There are 3 possible cases:
//
// :___ ___:    :___ _:_ ___:    :___ __: ___ :__ ___:
// |___|___|    |___|_:_|___|    |___|__:|___|:__|___|
// :       :    :     :     :    :      :     :      :
//
// 1) No partial coverage, max window = scaleFactor;
// 2) Partial coverage occurs on the one side of the output pixel,
//    max window = scaleFactor + 1;
// 3) Partial coverage occurs at both sides of the output pixel,
//    max window = scaleFactor + 2;
//
// Type of the coverage is determined by remainder of
// inPeriodH/outPeriodH division, but it's an heuristic
// (howbeit didn't found the proof of the opposite so far).

static int calcResizeWindow(int inH, int outH)
{
    CV_Assert(inH >= outH);
    auto gcd = calcGcd(inH, outH);
    int  inPeriodH =  inH/gcd;
    int outPeriodH = outH/gcd;
    int scaleFactor = inPeriodH / outPeriodH;

    switch ((inPeriodH) % (outPeriodH))
    {
    case 0:  return scaleFactor;     break;
    case 1:  return scaleFactor + 1; break;
    default: return scaleFactor + 2;
    }
}

static int maxReadWindow(const cv::GFluidKernel& k, int inH, int outH)
{
    switch (k.m_kind)
    {
    case cv::GFluidKernel::Kind::Filter: return k.m_window; break;
    case cv::GFluidKernel::Kind::Resize:
    {
        if  (inH >= outH)
        {
            return calcResizeWindow(inH, outH);
        }
        else
        {
            // Upscale always has window of 2
            return (inH == 1) ? 1 : 2;
        }
    } break;
    default: CV_Assert(false); return 0;
    }
}

static int borderSize(const cv::GFluidKernel& k)
{
    switch (k.m_kind)
    {
    case cv::GFluidKernel::Kind::Filter: return (k.m_window - 1) / 2; break;
    // Resize never reads from border pixels
    case cv::GFluidKernel::Kind::Resize: return 0; break;
    default: CV_Assert(false); return 0;
    }
}

double inCoord(int outIdx, double ratio)
{
    return outIdx * ratio;
}

int windowStart(int outIdx, double ratio)
{
    return static_cast<int>(inCoord(outIdx, ratio) + 1e-3);
}

int windowEnd(int outIdx, double ratio)
{
    return static_cast<int>(std::ceil(inCoord(outIdx + 1, ratio) - 1e-3));
}

double inCoordUpscale(int outCoord, double ratio)
{
    // Calculate the projection of output pixel's center
    return (outCoord + 0.5) * ratio - 0.5;
}

int upscaleWindowStart(int outCoord, double ratio)
{
    int start = static_cast<int>(inCoordUpscale(outCoord, ratio));
    CV_Assert(start >= 0);
    return start;
}

int upscaleWindowEnd(int outCoord, double ratio, int inSz)
{
    int end = static_cast<int>(std::ceil(inCoordUpscale(outCoord, ratio)) + 1);
    if (end > inSz)
    {
        end = inSz;
    }
    return end;
}
} // anonymous namespace

int cv::gimpl::FluidFilterAgent::firstWindow() const
{
    return k.m_window + k.m_lpi - 1;
}

int cv::gimpl::FluidFilterAgent::nextWindow() const
{
    int lpi = std::min(k.m_lpi, m_outputLines - m_producedLines - k.m_lpi);
    return k.m_window - 1 + lpi;
}

int cv::gimpl::FluidFilterAgent::linesRead() const
{
    return k.m_lpi;
}

int cv::gimpl::FluidResizeAgent::firstWindow() const
{
    auto outIdx = out_buffers[0]->priv().y();
    return windowEnd(outIdx, m_ratio) - windowStart(outIdx, m_ratio);
}

int cv::gimpl::FluidResizeAgent::nextWindow() const
{
    auto outIdx = out_buffers[0]->priv().y();
    return windowEnd(outIdx + 1, m_ratio) - windowStart(outIdx + 1, m_ratio);
}

int cv::gimpl::FluidResizeAgent::linesRead() const
{
    auto outIdx = out_buffers[0]->priv().y();
    return windowStart(outIdx + 1, m_ratio) - windowStart(outIdx, m_ratio);
}

int cv::gimpl::FluidUpscaleAgent::firstWindow() const
{
    auto outIdx = out_buffers[0]->priv().y();
    return upscaleWindowEnd(outIdx, m_ratio, in_views[0].meta().size.height) - upscaleWindowStart(outIdx, m_ratio);
}

int cv::gimpl::FluidUpscaleAgent::nextWindow() const
{
    auto outIdx = out_buffers[0]->priv().y();
    return upscaleWindowEnd(outIdx + 1, m_ratio, in_views[0].meta().size.height) - upscaleWindowStart(outIdx + 1, m_ratio);
}

int cv::gimpl::FluidUpscaleAgent::linesRead() const
{
    auto outIdx = out_buffers[0]->priv().y();
    return upscaleWindowStart(outIdx + 1, m_ratio) - upscaleWindowStart(outIdx, m_ratio);
}

bool cv::gimpl::FluidAgent::canRead() const
{
    // An agent can work if every input buffer have enough data to start
    for (auto in_view : in_views)
    {
        if (in_view)
        {
            if (!in_view.ready())
                return false;
        }
    }
    return true;
}

bool cv::gimpl::FluidAgent::canWrite() const
{
    // An agent can work if there is space to write in its output
    // allocated buffers
    CV_Assert(!out_buffers.empty());
    auto out_begin = out_buffers.begin();
    auto out_end   = out_buffers.end();
    if (k.m_scratch) out_end--;
    for (auto it = out_begin; it != out_end; ++it)
    {
        if ((*it)->priv().full())
        {
            return false;
        }
    }
    return true;
}

bool cv::gimpl::FluidAgent::canWork() const
{
    return canRead() && canWrite();
}

void cv::gimpl::FluidAgent::doWork()
{
    GAPI_Assert(m_outputLines > m_producedLines);
    for (auto in_view : in_views)
    {
        if (in_view) in_view.priv().prepareToRead();
    }

    k.m_f(in_args, out_buffers);

    for (auto in_view : in_views)
    {
        if (in_view) in_view.priv().readDone(linesRead(), nextWindow());
    }

    for (auto out_buf : out_buffers)
    {
        out_buf->priv().writeDone();
        // FIXME WARNING: Scratch buffers rotated here too!
    }

    m_producedLines += k.m_lpi;
}

bool cv::gimpl::FluidAgent::done() const
{
    // m_producedLines is a multiple of LPI, while original
    // height may be not.
    return m_producedLines >= m_outputLines;
}

void cv::gimpl::FluidAgent::debug(std::ostream &os)
{
    os << "Fluid Agent " << std::hex << this
       << " (" << op_name << ") --"
       << " canWork=" << std::boolalpha << canWork()
       << " canRead=" << std::boolalpha << canRead()
       << " canWrite=" << std::boolalpha << canWrite()
       << " done="    << done()
       << " lines="   << std::dec << m_producedLines << "/" << m_outputLines
       << " {{\n";
    for (auto out_buf : out_buffers)
    {
        out_buf->debug(os);
    }
    std::cout << "}}" << std::endl;
}

// GCPUExcecutable implementation //////////////////////////////////////////////
cv::gimpl::GFluidExecutable::GFluidExecutable(const ade::Graph &g,
                                              const std::vector<ade::NodeHandle> &nodes,
                                              const std::vector<cv::gapi::own::Rect> &outputRois)
    : m_g(g), m_gm(m_g), m_outputRois(outputRois)
{
    GConstFluidModel fg(m_g);

    // Initialize vector of data buffers, build list of operations
    // FIXME: There _must_ be a better way to [query] count number of DATA nodes
    std::size_t mat_count = 0;
    std::size_t last_agent = 0;
    std::map<std::size_t, ade::NodeHandle> all_gmat_ids;

    auto grab_mat_nh = [&](ade::NodeHandle nh) {
        auto rc = m_gm.metadata(nh).get<Data>().rc;
        if (m_id_map.count(rc) == 0)
        {
            all_gmat_ids[mat_count] = nh;
            m_id_map[rc] = mat_count++;
        }
    };

    for (const auto &nh : nodes)
    {
        switch (m_gm.metadata(nh).get<NodeType>().t)
        {
        case NodeType::DATA:
            if (m_gm.metadata(nh).get<Data>().shape == GShape::GMAT)
                grab_mat_nh(nh);
            break;

        case NodeType::OP:
        {
            const auto& fu = fg.metadata(nh).get<FluidUnit>();
            switch (fu.k.m_kind)
            {
            case GFluidKernel::Kind::Filter: m_agents.emplace_back(new FluidFilterAgent(m_g, nh)); break;
            case GFluidKernel::Kind::Resize:
            {
                if (fu.ratio >= 1.0)
                {
                    m_agents.emplace_back(new FluidResizeAgent(m_g, nh));
                }
                else
                {
                    m_agents.emplace_back(new FluidUpscaleAgent(m_g, nh));
                }
            } break;
            default: CV_Assert(false);
            }
            // NB.: in_buffer_ids size is equal to Arguments size, not Edges size!!!
            m_agents.back()->in_buffer_ids.resize(m_gm.metadata(nh).get<Op>().args.size(), -1);
            for (auto eh : nh->inEdges())
            {
                // FIXME Only GMats are currently supported (which can be represented
                // as fluid buffers
                if (m_gm.metadata(eh->srcNode()).get<Data>().shape == GShape::GMAT)
                {
                    const auto in_port = m_gm.metadata(eh).get<Input>().port;
                    const auto in_buf  = m_gm.metadata(eh->srcNode()).get<Data>().rc;

                    m_agents.back()->in_buffer_ids[in_port] = in_buf;
                    grab_mat_nh(eh->srcNode());
                }
            }
            // FIXME: Assumption that all operation outputs MUST be connected
            m_agents.back()->out_buffer_ids.resize(nh->outEdges().size(), -1);
            for (auto eh : nh->outEdges())
            {
                const auto& data = m_gm.metadata(eh->dstNode()).get<Data>();
                const auto out_port = m_gm.metadata(eh).get<Output>().port;
                const auto out_buf  = data.rc;

                m_agents.back()->out_buffer_ids[out_port] = out_buf;
                if (data.shape == GShape::GMAT) grab_mat_nh(eh->dstNode());
            }
            if (fu.k.m_scratch)
                m_scratch_users.push_back(last_agent);
            last_agent++;
            break;
        }
        default: GAPI_Assert(false);
        }
    }

    // Check that IDs form a continiuos set (important for further indexing)
    GAPI_Assert(m_id_map.size() >  0u);
    GAPI_Assert(m_id_map.size() == mat_count);

    // Actually initialize Fluid buffers
    GAPI_LOG_INFO(NULL, "Initializing " << mat_count << " fluid buffer(s)" << std::endl);
    m_num_int_buffers = mat_count;
    const std::size_t num_scratch = m_scratch_users.size();

    // Calculate rois for each fluid buffer

    auto proto = m_gm.metadata().get<Protocol>();
    std::vector<int> readStarts(mat_count);
    std::vector<cv::gapi::own::Rect> rois(mat_count);
    std::stack<ade::NodeHandle> nodesToVisit;

    if (proto.outputs.size() != m_outputRois.size())
    {
        CV_Assert(m_outputRois.size() == 0);
        m_outputRois.resize(proto.outputs.size());
    }

    // First, initialize rois for output nodes, add them to traversal stack
    for (const auto& it : ade::util::indexed(proto.out_nhs))
    {
        const auto idx = ade::util::index(it);
        const auto nh  = ade::util::value(it);

        const auto &d  = m_gm.metadata(nh).get<Data>();

        // This is not our output
        if (m_id_map.count(d.rc) == 0)
        {
            continue;
        }

        if (d.shape == GShape::GMAT)
        {
            auto desc = util::get<GMatDesc>(d.meta);
            if (m_outputRois[idx] == cv::gapi::own::Rect{})
            {
                m_outputRois[idx] = cv::gapi::own::Rect{0, 0, desc.size.width, desc.size.height};
            }

            // Only slices are supported at the moment
            GAPI_Assert(m_outputRois[idx].x == 0);
            GAPI_Assert(m_outputRois[idx].width == desc.size.width);

            auto id = m_id_map.at(d.rc);
            readStarts[id] = 0;
            rois[id] = m_outputRois[idx];
            nodesToVisit.push(nh);
        }
    }

    // Perform a wide search from each of the output nodes
    // And extend roi of buffers by border_size
    // Each node can be visited multiple times
    // (if node has been already visited, the check that inferred rois are the same is performed)
    while (!nodesToVisit.empty())
    {
        const auto startNode = nodesToVisit.top();
        nodesToVisit.pop();

        if (!startNode->inNodes().empty())
        {
            GAPI_Assert(startNode->inNodes().size() == 1);
            const auto& oh = startNode->inNodes().front();
            const auto& data = m_gm.metadata(startNode).get<Data>();
            // only GMats participate in the process so it's valid to obtain GMatDesc
            const auto& meta = util::get<GMatDesc>(data.meta);

            for (const auto& inNode : oh->inNodes())
            {
                const auto& in_data = m_gm.metadata(inNode).get<Data>();

                if (in_data.shape == GShape::GMAT)
                {
                    const auto& in_meta = util::get<GMatDesc>(in_data.meta);
                    const auto& fd = fg.metadata(inNode).get<FluidData>();

                    auto adjFilterRoi = [](cv::gapi::own::Rect produced, int b, int max_height) {
                        // Extend with border roi which should be produced, crop to logical image size
                        cv::gapi::own::Rect roi = {produced.x, produced.y - b, produced.width, produced.height + 2*b};
                        cv::gapi::own::Rect fullImg{ 0, 0, produced.width, max_height };
                        return roi & fullImg;
                    };

                    auto adjResizeRoi = [](cv::gapi::own::Rect produced, cv::gapi::own::Size inSz, cv::gapi::own::Size outSz) {
                        auto map = [](int outCoord, int producedSz, int inSize, int outSize) {
                            double ratio = (double)inSize / outSize;
                            int w0 = 0, w1 = 0;
                            if (ratio >= 1.0)
                            {
                                w0 = windowStart(outCoord, ratio);
                                w1 = windowEnd  (outCoord + producedSz - 1, ratio);
                            }
                            else
                            {
                                w0 = upscaleWindowStart(outCoord, ratio);
                                w1 = upscaleWindowEnd(outCoord + producedSz - 1, ratio, inSize);
                            }
                            return std::make_pair(w0, w1);
                        };

                        auto mapY = map(produced.y, produced.height, inSz.height, outSz.height);
                        auto y0 = mapY.first;
                        auto y1 = mapY.second;

                        auto mapX = map(produced.x, produced.width, inSz.width, outSz.width);
                        auto x0 = mapX.first;
                        auto x1 = mapX.second;

                        cv::gapi::own::Rect roi = {x0, y0, x1 - x0, y1 - y0};
                        return roi;
                    };

                    cv::gapi::own::Rect produced = rois[m_id_map.at(data.rc)];

                    cv::gapi::own::Rect resized;
                    switch (fg.metadata(oh).get<FluidUnit>().k.m_kind)
                    {
                    case GFluidKernel::Kind::Filter: resized = produced; break;
                    case GFluidKernel::Kind::Resize: resized = adjResizeRoi(produced, in_meta.size, meta.size); break;
                    default: CV_Assert(false);
                    }

                    int readStart = resized.y;
                    cv::gapi::own::Rect roi = adjFilterRoi(resized, fd.border_size, in_meta.size.height);

                    auto in_id = m_id_map.at(in_data.rc);
                    if (rois[in_id] == cv::gapi::own::Rect{})
                    {
                        readStarts[in_id] = readStart;
                        rois[in_id] = roi;
                        nodesToVisit.push(inNode);
                    }
                    else
                    {
                        GAPI_Assert(readStarts[in_id] == readStart);
                        GAPI_Assert(rois[in_id] == roi);
                    }
                } // if (in_data.shape == GShape::GMAT)
            } // for (const auto& inNode : oh->inNodes())
        } // if (!startNode->inNodes().empty())
    } // while (!nodesToVisit.empty())

    // NB: Allocate ALL buffer object at once, and avoid any further reallocations
    // (since raw pointers-to-elements are taken)
    m_buffers.resize(m_num_int_buffers + num_scratch);
    for (const auto &it : all_gmat_ids)
    {
        auto id = it.first;
        auto nh = it.second;
        const auto & d  = m_gm.metadata(nh).get<Data>();
        const auto &fd  = fg.metadata(nh).get<FluidData>();
        const auto meta = cv::util::get<GMatDesc>(d.meta);

        // FIXME: Only continuous set...
        m_buffers[id].priv().init(meta, fd.max_consumption, fd.border_size, fd.skew, fd.lpi_write, readStarts[id], rois[id]);

        if (d.storage == Data::Storage::INTERNAL)
        {
            m_buffers[id].priv().allocate(fd.border);
            std::stringstream stream;
            m_buffers[id].debug(stream);
            GAPI_LOG_INFO(NULL, stream.str());
        }
    }

    // After buffers are allocated, repack: ...
    for (auto &agent : m_agents)
    {
        // a. Agent input parameters with View pointers (creating Views btw)
        const auto &op = m_gm.metadata(agent->op_handle).get<Op>();
        const auto &fu =   fg.metadata(agent->op_handle).get<FluidUnit>();
        agent->in_args.resize(op.args.size());
        agent->in_views.resize(op.args.size());
        for (auto it : ade::util::zip(ade::util::iota(op.args.size()),
                                      ade::util::toRange(agent->in_buffer_ids)))
        {
            auto in_idx  = std::get<0>(it);
            auto buf_idx = std::get<1>(it);

            if (buf_idx >= 0)
            {
                // IF there is input buffer, register a view (every unique
                // reader has its own), and store it in agent Args
                gapi::fluid::Buffer &buffer = m_buffers.at(m_id_map.at(buf_idx));

                auto inEdge = GModel::getInEdgeByPort(m_g, agent->op_handle, in_idx);
                auto ownStorage = fg.metadata(inEdge).get<FluidUseOwnBorderBuffer>().use;

                gapi::fluid::View view = buffer.mkView(fu.line_consumption, fu.border_size, fu.border, ownStorage);
                // NB: It is safe to keep ptr as view lifetime is buffer lifetime
                agent->in_views[in_idx] = view;
                agent->in_args[in_idx]  = GArg(view);
                agent->m_ratio = fu.ratio;
            }
            else
            {
                // Copy(FIXME!) original args as is
                agent->in_args[in_idx] = op.args[in_idx];
            }
        }

        // b. Agent output parameters with Buffer pointers.
        agent->out_buffers.resize(agent->op_handle->outEdges().size(), nullptr);
        for (auto it : ade::util::zip(ade::util::iota(agent->out_buffers.size()),
                                      ade::util::toRange(agent->out_buffer_ids)))
        {
            auto out_idx = std::get<0>(it);
            auto buf_idx = m_id_map.at(std::get<1>(it));
            agent->out_buffers.at(out_idx) = &m_buffers.at(buf_idx);
            agent->m_outputLines = m_buffers.at(buf_idx).priv().outputLines();
        }
    }

    // After parameters are there, initialize scratch buffers
    if (num_scratch)
    {
        GAPI_LOG_INFO(NULL, "Initializing " << num_scratch << " scratch buffer(s)" << std::endl);
        unsigned last_scratch_id = 0;

        for (auto i : m_scratch_users)
        {
            auto &agent = m_agents.at(i);
            GAPI_Assert(agent->k.m_scratch);

            // Collect input metas to trigger scratch buffer initialization
            // Array is sparse (num of elements == num of GArgs, not edges)
            GMetaArgs in_metas(agent->in_args.size());
            for (auto eh : agent->op_handle->inEdges())
            {
                const auto& in_data = m_gm.metadata(eh->srcNode()).get<Data>();
                in_metas[m_gm.metadata(eh).get<Input>().port] = in_data.meta;
            }

            // Trigger Scratch buffer initialization method
            const std::size_t new_scratch_idx = m_num_int_buffers + last_scratch_id;

            agent->k.m_is(in_metas, agent->in_args, m_buffers.at(new_scratch_idx));
            std::stringstream stream;
            m_buffers[new_scratch_idx].debug(stream);
            GAPI_LOG_INFO(NULL, stream.str());
            agent->out_buffers.emplace_back(&m_buffers[new_scratch_idx]);
            last_scratch_id++;
        }
    }

    int total_size = 0;
    for (const auto &i : ade::util::indexed(m_buffers))
    {
        // Check that all internal and scratch buffers are allocated
        auto idx = ade::util::index(i);
        auto b   = ade::util::value(i);
        if (idx >= m_num_int_buffers ||
            m_gm.metadata(all_gmat_ids[idx]).get<Data>().storage == Data::Storage::INTERNAL)
        {
            GAPI_Assert(b.priv().size() > 0);
        }

        // Buffers which will be bound to real images may have size of 0 at this moment
        // (There can be non-zero sized const border buffer allocated in such buffers)
        total_size += b.priv().size();
    }
    GAPI_LOG_INFO(NULL, "Internal buffers: " << std::fixed << std::setprecision(2) << static_cast<float>(total_size)/1024 << " KB\n");
}

// FIXME: Document what it does
void cv::gimpl::GFluidExecutable::bindInArg(const cv::gimpl::RcDesc &rc, const GRunArg &arg)
{
    switch (rc.shape)
    {
    case GShape::GMAT:    m_buffers[m_id_map.at(rc.id)].priv().bindTo(to_ocv(util::get<cv::gapi::own::Mat>(arg)), true); break;
    case GShape::GSCALAR: m_res.slot<cv::gapi::own::Scalar>()[rc.id] = util::get<cv::gapi::own::Scalar>(arg); break;
    default: util::throw_error(std::logic_error("Unsupported GShape type"));
    }
}

void cv::gimpl::GFluidExecutable::bindOutArg(const cv::gimpl::RcDesc &rc, const GRunArgP &arg)
{
    // Only GMat is supported as return type
    switch (rc.shape)
    {
    case GShape::GMAT:
        {
            cv::GMatDesc desc = m_buffers[m_id_map.at(rc.id)].meta();
            auto      &outMat = *util::get<cv::gapi::own::Mat*>(arg);
            GAPI_Assert(outMat.data != nullptr);
            GAPI_Assert(descr_of(outMat) == desc && "Output argument was not preallocated as it should be ?");
            m_buffers[m_id_map.at(rc.id)].priv().bindTo(to_ocv(outMat), false);
            break;
        }
    default: util::throw_error(std::logic_error("Unsupported return GShape type"));
    }
}

void cv::gimpl::GFluidExecutable::packArg(cv::GArg &in_arg, const cv::GArg &op_arg)
{
    GAPI_Assert(op_arg.kind != cv::detail::ArgKind::GMAT
           && op_arg.kind != cv::detail::ArgKind::GSCALAR);

    if (op_arg.kind == cv::detail::ArgKind::GOBJREF)
    {
        const cv::gimpl::RcDesc &ref = op_arg.get<cv::gimpl::RcDesc>();
        if (ref.shape == GShape::GSCALAR)
        {
            in_arg = GArg(m_res.slot<cv::gapi::own::Scalar>()[ref.id]);
        }
    }
}

void cv::gimpl::GFluidExecutable::run(std::vector<InObj>  &&input_objs,
                                      std::vector<OutObj> &&output_objs)
{
    // Bind input buffers from parameters
    for (auto& it : input_objs)  bindInArg(it.first, it.second);
    for (auto& it : output_objs) bindOutArg(it.first, it.second);

    // Reset Buffers and Agents state before we go
    for (auto &buffer : m_buffers)
        buffer.priv().reset();

    for (auto &agent : m_agents)
    {
        agent->reset();
        // Pass input cv::Scalar's to agent argument
        const auto& op = m_gm.metadata(agent->op_handle).get<Op>();
        for (const auto& it : ade::util::indexed(op.args))
        {
            const auto& arg = ade::util::value(it);
            packArg(agent->in_args[ade::util::index(it)], arg);
        }
    }

    // Explicitly reset Scratch buffers, if any
    for (auto scratch_i : m_scratch_users)
    {
        auto &agent = m_agents[scratch_i];
        GAPI_Assert(agent->k.m_scratch);
        agent->k.m_rs(*agent->out_buffers.back());
    }

    // Now start executing our stuff!
    // Fluid execution is:
    // - run through list of Agents from Left to Right
    // - for every Agent:
    //   - if all input Buffers have enough data to fulfill
    //     Agent's window - trigger Agent
    //     - on trigger, Agent takes all input lines from input buffers
    //       and produces a single output line
    //     - once Agent finishes, input buffers get "readDone()",
    //       and output buffers get "writeDone()"
    //   - if there's not enough data, Agent is skipped
    // Yes, THAT easy!
    bool complete = true;
    do {
        complete = true;
        bool work_done=false;
        for (auto &agent : m_agents)
        {
            // agent->debug(std::cout);
            if (!agent->done())
            {
                if (agent->canWork())
                {
                    agent->doWork(); work_done=true;
                }
                if (!agent->done())   complete = false;
            }
        }
        GAPI_Assert(work_done || complete);
    } while (!complete); // FIXME: number of iterations can be calculated statically
}

// FIXME: these passes operate on graph global level!!!
// Need to fix this for heterogeneous (island-based) processing
void GFluidBackendImpl::addBackendPasses(ade::ExecutionEngineSetupContext &ectx)
{
    using namespace cv::gimpl;

    // FIXME: all passes were moved to "exec" stage since Fluid
    // should check Islands configuration first (which is now quite
    // limited), and only then continue with all other passes.
    //
    // The passes/stages API must be streamlined!
    ectx.addPass("exec", "fluid_sanity_check", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        auto isl_graph = g.metadata().get<IslandModel>().model;
        GIslandModel::Graph gim(*isl_graph);

        const auto num_non_fluid_islands = std::count_if
            (gim.nodes().begin(),
             gim.nodes().end(),
             [&](const ade::NodeHandle &nh) {
                return gim.metadata(nh).get<NodeKind>().k == NodeKind::ISLAND &&
                       gim.metadata(nh).get<FusedIsland>().object->backend() != cv::gapi::fluid::backend();
            });

        // FIXME: Break this limitation!
        if (num_non_fluid_islands > 0)
            cv::util::throw_error(std::logic_error("Fluid doesn't support heterogeneous execution"));
    });
    ectx.addPass("exec", "init_fluid_data", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);
        for (const auto node : g.nodes())
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::DATA)
            {
                fg.metadata(node).set(FluidData());
            }
        }
    });
    // FIXME:
    // move to unpackKernel method
    // when https://gitlab-icv.inn.intel.com/G-API/g-api/merge_requests/66 is merged
    ectx.addPass("exec", "init_fluid_unit_borders", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);

        auto sorted = g.metadata().get<ade::passes::TopologicalSortData>().nodes();
        for (auto node : sorted)
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::OP)
            {
                // FIXME: check that op has only one data node on input
                auto &fu = fg.metadata(node).get<FluidUnit>();
                const auto &op = g.metadata(node).get<Op>();

                // Trigger user-defined "getBorder" callback
                fu.border = fu.k.m_b(GModel::collectInputMeta(fg, node), op.args);
            }
        }
    });
    ectx.addPass("exec", "init_fluid_units", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);

        auto sorted = g.metadata().get<ade::passes::TopologicalSortData>().nodes();
        for (auto node : sorted)
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::OP)
            {
                std::set<int> in_hs, out_ws, out_hs;

                for (const auto& in : node->inNodes())
                {
                    const auto& d = g.metadata(in).get<Data>();
                    if (d.shape == cv::GShape::GMAT)
                    {
                        const auto& meta = cv::util::get<cv::GMatDesc>(d.meta);
                        in_hs.insert(meta.size.height);
                    }
                }

                for (const auto& out : node->outNodes())
                {
                    const auto& d = g.metadata(out).get<Data>();
                    if (d.shape == cv::GShape::GMAT)
                    {
                        const auto& meta = cv::util::get<cv::GMatDesc>(d.meta);
                        out_ws.insert(meta.size.width);
                        out_hs.insert(meta.size.height);
                    }
                }

                CV_Assert(in_hs.size() == 1 && out_ws.size() == 1 && out_hs.size() == 1);

                auto in_h  = *in_hs .cbegin();
                auto out_h = *out_hs.cbegin();

                auto &fu = fg.metadata(node).get<FluidUnit>();
                fu.ratio = (double)in_h / out_h;

                int w = maxReadWindow(fu.k, in_h, out_h);
                int line_consumption = fu.k.m_lpi + w - 1;
                int border_size = borderSize(fu.k);

                fu.border_size = border_size;
                fu.line_consumption = line_consumption;

                GModel::log(g, node, "Line consumption: " + std::to_string(fu.line_consumption));
                GModel::log(g, node, "Border size: " + std::to_string(fu.border_size));
            }
        }
    });
    ectx.addPass("exec", "init_line_consumption", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);
        for (const auto node : g.nodes())
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::OP)
            {
                const auto &fu = fg.metadata(node).get<FluidUnit>();

                for (auto in_data_node : node->inNodes())
                {
                    auto &fd = fg.metadata(in_data_node).get<FluidData>();

                    // Update (not Set) fields here since a single data node may be
                    // accessed by multiple consumers
                    fd.max_consumption = std::max(fu.line_consumption, fd.max_consumption);
                    fd.border_size     = std::max(fu.border_size, fd.border_size);

                    GModel::log(g, in_data_node, "Line consumption: " + std::to_string(fd.max_consumption)
                                + " (upd by " + std::to_string(fu.line_consumption) + ")", node);
                    GModel::log(g, in_data_node, "Border size: " + std::to_string(fd.border_size), node);
                }
            }
        }
    });
    ectx.addPass("exec", "calc_latency", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);

        auto sorted = g.metadata().get<ade::passes::TopologicalSortData>().nodes();
        for (auto node : sorted)
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::OP)
            {
                const auto &fu = fg.metadata(node).get<FluidUnit>();

                const int own_latency = fu.line_consumption - fu.border_size;
                GModel::log(g, node, "LPI: " + std::to_string(fu.k.m_lpi));

                // Output latency is max(input_latency) + own_latency
                int in_latency = 0;
                for (auto in_data_node : node->inNodes())
                {
                    // FIXME: ASSERT(DATA), ASSERT(FLUIDDATA)
                    in_latency = std::max(in_latency, fg.metadata(in_data_node).get<FluidData>().latency);
                }
                const int out_latency = in_latency + own_latency;

                for (auto out_data_node : node->outNodes())
                {
                    // FIXME: ASSERT(DATA), ASSERT(FLUIDDATA)
                    auto &fd     = fg.metadata(out_data_node).get<FluidData>();
                    fd.latency   = out_latency;
                    fd.lpi_write = fu.k.m_lpi;
                    GModel::log(g, out_data_node, "Latency: " + std::to_string(out_latency));
                }
            }
        }
    });
    ectx.addPass("exec", "calc_skew", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);

        auto sorted = g.metadata().get<ade::passes::TopologicalSortData>().nodes();
        for (auto node : sorted)
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::OP)
            {
                int max_latency = 0;
                for (auto in_data_node : node->inNodes())
                {
                    // FIXME: ASSERT(DATA), ASSERT(FLUIDDATA)
                    max_latency = std::max(max_latency, fg.metadata(in_data_node).get<FluidData>().latency);
                }
                for (auto in_data_node : node->inNodes())
                {
                    // FIXME: ASSERT(DATA), ASSERT(FLUIDDATA)
                    auto &fd = fg.metadata(in_data_node).get<FluidData>();

                    // Update (not Set) fields here since a single data node may be
                    // accessed by multiple consumers
                    fd.skew = std::max(fd.skew, max_latency - fd.latency);

                    GModel::log(g, in_data_node, "Skew: " + std::to_string(fd.skew), node);
                }
            }
        }
    });
    ectx.addPass("exec", "init_buffer_borders", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);
        auto sorted = g.metadata().get<ade::passes::TopologicalSortData>().nodes();
        for (auto node : sorted)
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::DATA)
            {
                auto &fd = fg.metadata(node).get<FluidData>();

                // Assign border stuff to FluidData

                // In/out data nodes are bound to user data directly,
                // so cannot be extended with a border
                if (g.metadata(node).get<Data>().storage == Data::Storage::INTERNAL)
                {
                    // For now border of the buffer's storage is the border
                    // of the first reader whose border size is the same.
                    // FIXME: find more clever strategy of border picking
                    // (it can be a border which is common for majority of the
                    // readers, also we can calculate the number of lines which
                    // will be copied by views on each iteration and base our choice
                    // on this criteria)
                    auto readers = node->outNodes();
                    const auto &candidate = ade::util::find_if(readers, [&](ade::NodeHandle nh) {
                        const auto &fu = fg.metadata(nh).get<FluidUnit>();
                        return fu.border_size == fd.border_size;
                    });
                    GAPI_Assert(candidate != readers.end());

                    const auto &fu = fg.metadata(*candidate).get<FluidUnit>();
                    fd.border = fu.border;
                }

                if (fd.border)
                {
                    GModel::log(g, node, "Border type: " + std::to_string(fd.border->type), node);
                }
            }
        }
    });
    ectx.addPass("exec", "init_view_borders", [](ade::passes::PassContext &ctx)
    {
        GModel::Graph g(ctx.graph);
        if (!GModel::isActive(g, cv::gapi::fluid::backend()))  // FIXME: Rearchitect this!
            return;

        GFluidModel fg(ctx.graph);
        for (auto node : g.nodes())
        {
            if (g.metadata(node).get<NodeType>().t == NodeType::DATA)
            {
                auto &fd = fg.metadata(node).get<FluidData>();
                for (auto out_edge : node->outEdges())
                {
                    const auto &fu = fg.metadata(out_edge->dstNode()).get<FluidUnit>();

                    // There is no need in own storage for view if it's border is
                    // the same as the buffer's (view can have equal or smaller border
                    // size in this case)
                    if (fu.border_size == 0 ||
                        (fu.border && fd.border && (*fu.border == *fd.border)))
                    {
                        GAPI_Assert(fu.border_size <= fd.border_size);
                        fg.metadata(out_edge).set(FluidUseOwnBorderBuffer{false});
                    }
                    else
                    {
                        fg.metadata(out_edge).set(FluidUseOwnBorderBuffer{true});
                        GModel::log(g, out_edge, "OwnBufferStorage: true");
                    }
                }
            }
        }
    });
}
