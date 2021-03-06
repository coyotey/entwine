/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include <pdal/PointView.hpp>

#include <entwine/third/arbiter/arbiter.hpp>
#include <entwine/types/bounds.hpp>
#include <entwine/types/dir.hpp>
#include <entwine/types/defs.hpp>
#include <entwine/types/metadata.hpp>
#include <entwine/util/pool.hpp>

namespace arbiter
{
    class Endpoint;
}

namespace entwine
{

class Traverser;
class Tile;
class Tiler;

class Above
{
public:
    Above(
            const Id& chunkId,
            const Bounds& bounds,
            const Schema& schema,
            std::size_t delta)
        : m_chunkId(chunkId)
        , m_bounds(bounds)
        , m_schema(schema)
        , m_delta(delta)
        , m_segments()
        , m_here(false)
    { }

    virtual ~Above()
    {
        // TODO If this is a transformation, smash the segments together and
        // then write out this chunk.
    }

    using Map = std::map<Bounds, std::unique_ptr<Above>>;
    using MapVal = Map::value_type;
    using Set = std::set<Above*>;

    const Id& chunkId() const { return m_chunkId; }
    bool here() const { return m_here; }

    virtual void populate(std::unique_ptr<std::vector<char>> data);

    const std::vector<char>* data(const Bounds& bounds) const
    {
        if (m_segments.count(bounds)) return &m_segments.at(bounds);
        else return nullptr;
    }

protected:
    const Id m_chunkId;
    const Bounds m_bounds;
    const Schema& m_schema;
    const std::size_t m_delta;
    std::map<Bounds, std::vector<char>> m_segments;
    bool m_here;
};

class Base : public Above
{
public:
    Base(const Tiler& tiler);

    virtual ~Base() override
    {
        // TODO Reserialize base, if this is a transformation.  Something like:
        /*
        const Schema celledSchema(BaseChunk::makeCelled(m_metadata.schema()));
        auto toWrite(Compression::compress(*m_baseChunk, celledSchema));

        Chunk::pushTail(
                *toWrite,
                Chunk::Tail(
                    m_baseChunk->size() / celledSchema.pointSize(),
                    Chunk::Contiguous));

        m_outEndpoint->putSubpath(
                m_metadata.structure().baseIndexBegin().str(),
                *toWrite);
        */
    }

private:
    virtual void populate(std::unique_ptr<std::vector<char>> data) override;

    const Metadata& m_metadata;
    const Structure& m_structure;
};

class Tile
{
    using Belows = std::map<Id, std::unique_ptr<std::size_t>>;
    using Below = Belows::value_type;
    using Aboves = std::map<Above*, std::unique_ptr<std::size_t>>;

public:
    Tile(
            const Bounds& bounds,
            const Schema& schema,
            Above::Map& aboves,
            std::size_t maxPointsPerTile)
        : m_bounds(bounds)
        , m_schema(schema)
        , m_maxPointsPerTile(maxPointsPerTile)
        , m_aboves(getContainingFrom(m_bounds, aboves))
        , m_belows()
        , m_data()
        , m_owned(false)
    { }

    ~Tile()
    {
        // TODO If this is a transformation, write out each of the Belows.
        // Something like:
        /*
        if (m_outEndpoint)
        {
            auto toWrite(Compression::compress(data, schema));

            Chunk::pushTail(
                    *toWrite,
                    Chunk::Tail(
                        data.size() / schema.pointSize(),
                        Chunk::Contiguous)); // TODO Get from Structure.

            m_outEndpoint->putSubpath(id.str(), *toWrite);
        }
        */
    }

    void await(const Id& id) { m_belows[id] = nullptr; }
    void insert(const Id& id, std::unique_ptr<std::vector<char>> data)
    {
        m_belows.at(id).reset(new std::size_t(m_data.size()));
        m_data.insert(m_data.begin(), data->begin(), data->end());
    }

    // Returns true if the caller is cleared for processing this Tile.  If
    // the result is false, the caller should not process this Tile.
    bool acquire()
    {
        if (!m_owned && allHere())
        {
            m_owned = true;
            return true;
        }
        else
        {
            return false;
        }
    }

    bool allHere() const
    {
        auto aboveHere([](const Above* a) { return a->here(); });
        auto belowHere([](const Below& b) { return b.second.get(); });
        return
            std::all_of(m_aboves.begin(), m_aboves.end(), aboveHere) &&
            std::all_of(m_belows.begin(), m_belows.end(), belowHere);
    }

    void process(const TileFunction& f);

    bool references(Above& above) const { return m_aboves.count(&above); }

private:
    Above::Set getContainingFrom(const Bounds& bounds, Above::Map& aboves) const;

    void splitAndCall(
            const TileFunction& f,
            const std::vector<char>& data,
            const Bounds& bounds) const;

    const Bounds m_bounds;
    const Schema& m_schema;
    const std::size_t m_maxPointsPerTile;

    Above::Set m_aboves;
    Belows m_belows;

    std::vector<char> m_data;
    bool m_owned;
};

class Tiler
{
public:
    Tiler(
            const arbiter::Endpoint& inEndpoint,
            std::size_t threads,
            double maxTileWidth,
            const Schema* wantedSchema = nullptr,
            std::size_t maxPointsPerTile =
                std::numeric_limits<std::size_t>::max());

    /*
    Tiler(
            const Metadata& metadata,
            const arbiter::Endpoint& output,
            std::size_t threads,
            double maxTileWidth);
    */

    ~Tiler();

    void go(const TileFunction& f, const arbiter::Endpoint* ep = nullptr);

    const Metadata& metadata() const { return m_metadata; }
    const Schema* wantedSchema() const { return m_wantedSchema; }
    std::size_t sliceDepth() const { return m_sliceDepth; }

    const Schema& activeSchema() const;

    const arbiter::Endpoint& inEndpoint() const { return m_inEndpoint; }

private:
    void init(double maxTileWidth);

    void insertAbove(
            const TileFunction& f,
            const Id& chunkId,
            std::size_t depth,
            const Bounds& bounds);

    void spawnTile(
            const TileFunction& f,
            const Id& chunkId,
            const Bounds& bounds,
            bool exists);

    void buildTile(
            const TileFunction& f,
            const Id& chunkId,
            std::size_t depth,
            const Bounds& bounds);

    void awaitAndAcquire(
            const TileFunction& f,
            const Id& chunkId,
            Tile& tile);

    void maybeProcess(const TileFunction& f);
    std::unique_ptr<std::vector<char>> acquire(const Id& chunkId);
    using TileMap = std::map<Bounds, std::unique_ptr<Tile>>;

    const arbiter::Endpoint m_inEndpoint;

    const Metadata m_metadata;
    const std::set<Id> m_ids;
    const std::size_t m_maxPointsPerTile;

    std::unique_ptr<Traverser> m_traverser;
    mutable Pool m_pool;
    mutable std::mutex m_mutex;

    std::unique_ptr<Base> m_baseChunk;
    std::size_t m_sliceDepth;

    const Schema* const m_wantedSchema;

    std::map<Bounds, std::unique_ptr<Above>> m_aboves;
    TileMap m_tiles;

    std::unique_ptr<Bounds> m_current;
    std::set<Bounds> m_processing;
};

class SizedPointView : public pdal::PointView
{
public:
    template<typename Table>
    SizedPointView(Table& table) : PointView(table)
    {
        m_size = table.size();
        for (std::size_t i(0); i < m_size; ++i) m_index.push_back(i);
    }
};

} // namespace entwine

