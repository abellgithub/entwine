/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/tree/sleepy-tree.hpp>

#include <pdal/Dimension.hpp>
#include <pdal/Filter.hpp>
#include <pdal/PointView.hpp>
#include <pdal/Reader.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/StageWrapper.hpp>
#include <pdal/Utils.hpp>

#include <entwine/http/s3.hpp>
#include <entwine/tree/branch.hpp>
#include <entwine/tree/roller.hpp>
#include <entwine/tree/registry.hpp>
#include <entwine/tree/branches/clipper.hpp>
#include <entwine/types/bbox.hpp>
#include <entwine/types/linking-point-view.hpp>
#include <entwine/types/schema.hpp>
#include <entwine/types/simple-point-table.hpp>
#include <entwine/types/single-point-table.hpp>
#include <entwine/util/pool.hpp>
#include <entwine/util/fs.hpp>

namespace
{
    const std::size_t httpAttempts(3);
}

namespace entwine
{

SleepyTree::SleepyTree(
        const std::string& path,
        const BBox& bbox,
        const DimList& dimList,
        const S3Info& s3Info,
        const std::size_t numThreads,
        const std::size_t numDimensions,
        const std::size_t baseDepth,
        const std::size_t flatDepth,
        const std::size_t diskDepth)
    : m_path(path)
    , m_bbox(new BBox(bbox))
    , m_schema(new Schema(dimList))
    , m_originId(m_schema->pdalLayout().findDim("Origin"))
    , m_dimensions(numDimensions)
    , m_numPoints(0)
    , m_numTossed(0)
    , m_pool(new Pool(numThreads))
    , m_stageFactory(new pdal::StageFactory())
    , m_s3(new S3(s3Info))
    , m_registry(
            new Registry(
                m_path,
                *m_schema.get(),
                m_dimensions,
                baseDepth,
                flatDepth,
                diskDepth))
{
    if (m_dimensions != 2)
    {
        // TODO
        throw std::runtime_error("TODO - Only 2 dimensions so far");
    }
}

SleepyTree::SleepyTree(
        const std::string& path,
        const S3Info& s3Info,
        const std::size_t numThreads)
    : m_path(path)
    , m_bbox()
    , m_schema()
    , m_dimensions(0)
    , m_numPoints(0)
    , m_numTossed(0)
    , m_pool(new Pool(numThreads))
    , m_stageFactory(new pdal::StageFactory())
    , m_s3(new S3(s3Info))
    , m_registry()
{
    load();
}

SleepyTree::~SleepyTree()
{ }

void SleepyTree::insert(const std::string& filename)
{
    const Origin origin(addOrigin(filename));
    std::cout << "Adding " << origin << " - " << filename << std::endl;

    m_pool->add([this, origin, filename]()
    {
        const std::string driver(inferDriver(filename));
        const std::string localPath(fetchAndWriteFile(filename, origin));

        // Set up the file reader.
        std::unique_ptr<pdal::Reader> reader(
                static_cast<pdal::Reader*>(
                    m_stageFactory->createStage(driver)));

        reader->setSpatialReference(pdal::SpatialReference("EPSG:26915"));
        std::unique_ptr<pdal::Options> readerOptions(new pdal::Options());
        readerOptions->add(pdal::Option("filename", localPath));
        reader->setOptions(*readerOptions);

        // TODO Specify via config.
        // Set up the reprojection filter.
        std::shared_ptr<pdal::Filter> reproj(
                static_cast<pdal::Filter*>(
                    m_stageFactory->createStage("filters.reprojection")));

        std::unique_ptr<pdal::Options> reprojOptions(new pdal::Options());
        reprojOptions->add(
                pdal::Option(
                    "in_srs",
                    pdal::SpatialReference("EPSG:26915")));
        reprojOptions->add(
                pdal::Option(
                    "out_srs",
                    pdal::SpatialReference("EPSG:3857")));

        std::unique_ptr<SimplePointTable> pointTablePtr(
                new SimplePointTable(*m_schema));
        SimplePointTable& pointTable(*pointTablePtr);

        pdal::Filter& reprojRef(*reproj);

        pdal::FilterWrapper::initialize(reproj, pointTable);
        pdal::FilterWrapper::processOptions(reprojRef, *reprojOptions);
        pdal::FilterWrapper::ready(reprojRef, pointTable);

        // TODO Should pass to insert as reference, not ptr.
        std::unique_ptr<Clipper> clipper(new Clipper(*this));
        Clipper* clipperPtr(clipper.get());

        // Set up our per-point data handler.
        reader->setReadCb(
                [this, &pointTable, &reprojRef, origin, clipperPtr]
                (pdal::PointView& view, pdal::PointId index)
        {
            // TODO This won't work for dimension-oriented readers that
            // have partially written points after the given PointId.
            std::unique_ptr<LinkingPointView> link(
                new LinkingPointView(pointTable));
            pdal::FilterWrapper::filter(reprojRef, *link);

            insert(*link, origin, clipperPtr);
            pointTable.clear();
        });

        reader->prepare(pointTable);
        reader->execute(pointTable);

        std::cout << "\tDone " << origin << " - " << filename << std::endl;
        if (!fs::removeFile(localPath))
        {
            std::cout << "Couldn't delete " << localPath << std::endl;
            throw std::runtime_error("Couldn't delete tmp file");
        }
    });
}

void SleepyTree::insert(
        pdal::PointView& pointView,
        Origin origin,
        Clipper* clipper)
{
    Point point;

    for (std::size_t i = 0; i < pointView.size(); ++i)
    {
        point.x = pointView.getFieldAs<double>(pdal::Dimension::Id::X, i);
        point.y = pointView.getFieldAs<double>(pdal::Dimension::Id::Y, i);

        if (m_bbox->contains(point))
        {
            Roller roller(*m_bbox.get());

            pointView.setField(m_originId, i, origin);

            PointInfo* pointInfo(
                    new PointInfo(
                        new Point(point),
                        pointView.getPoint(i),
                        m_schema->pointSize()));

            if (m_registry->addPoint(&pointInfo, roller, clipper))
            {
                ++m_numPoints;
            }
            else
            {
                ++m_numTossed;
            }
        }
        else
        {
            ++m_numTossed;
        }
    }
}

void SleepyTree::join()
{
    m_pool->join();
}

void SleepyTree::clip(Clipper* clipper, std::size_t index)
{
    m_registry->clip(clipper, index);
}

void SleepyTree::save()
{
    // Ensure static state.
    join();

    // Get our own metadata.
    Json::Value jsonMeta(getTreeMeta());

    // Add the registry's metadata.
    m_registry->save(m_path, jsonMeta["registry"]);

    // Write to disk.
    fs::writeFile(
            metaPath(),
            jsonMeta.toStyledString(),
            std::ofstream::out | std::ofstream::trunc);
}

void SleepyTree::load()
{
    Json::Value meta;

    {
        Json::Reader reader;
        std::ifstream metaStream(metaPath());
        if (!metaStream.good())
        {
            throw std::runtime_error("Could not open " + metaPath());
        }

        reader.parse(metaStream, meta, false);
    }

    m_bbox.reset(new BBox(BBox::fromJson(meta["bbox"])));
    m_schema.reset(new Schema(Schema::fromJson(meta["schema"])));
    m_originId = m_schema->pdalLayout().findDim("Origin");
    m_dimensions = meta["dimensions"].asUInt64();
    m_numPoints = meta["numPoints"].asUInt64();
    m_numTossed = meta["numTossed"].asUInt64();
    const Json::Value& metaManifest(meta["manifest"]);

    for (Json::ArrayIndex i(0); i < metaManifest.size(); ++i)
    {
        m_originList.push_back(metaManifest[i].asString());
    }

    m_registry.reset(
            new Registry(
                m_path,
                *m_schema.get(),
                m_dimensions,
                meta["registry"]));
}

void SleepyTree::finalize(
        const S3Info& s3Info,
        const std::size_t base,
        const bool compress)
{
    join();

    std::unique_ptr<S3> output(new S3(s3Info));
    std::unique_ptr<std::vector<std::size_t>> ids(
            new std::vector<std::size_t>());

    const std::size_t baseEnd(Branch::calcOffset(base, m_dimensions));
    const std::size_t chunkPoints(
            baseEnd - Branch::calcOffset(base - 1, m_dimensions));

    {
        std::unique_ptr<Clipper> clipper(new Clipper(*this));

        std::shared_ptr<std::vector<char>> data(new std::vector<char>());

        for (std::size_t i(0); i < baseEnd; ++i)
        {
            std::vector<char> point(getPointData(clipper.get(), i, schema()));
            data->insert(data->end(), point.begin(), point.end());
        }

        output->put(std::to_string(0), data);
    }

    m_registry->finalize(*output, *m_pool, *ids, baseEnd, chunkPoints);

    {
        // Get our own metadata.
        Json::Value jsonMeta(getTreeMeta());
        jsonMeta["numIds"] = static_cast<Json::UInt64>(ids->size());
        jsonMeta["firstChunk"] = static_cast<Json::UInt64>(baseEnd);
        jsonMeta["chunkPoints"] = static_cast<Json::UInt64>(chunkPoints);
        output->put("entwine", jsonMeta.toStyledString());
    }

    Json::Value jsonIds;
    for (Json::ArrayIndex i(0); i < ids->size(); ++i)
    {
        jsonIds.append(static_cast<Json::UInt64>(ids->at(i)));
    }
    output->put("ids", jsonIds.toStyledString());
}

const BBox& SleepyTree::getBounds() const
{
    return *m_bbox.get();
}

std::vector<std::size_t> SleepyTree::query(
        Clipper* clipper,
        const std::size_t depthBegin,
        const std::size_t depthEnd)
{
    Roller roller(*m_bbox.get());
    std::vector<std::size_t> results;
    m_registry->query(roller, clipper, results, depthBegin, depthEnd);
    return results;
}

std::vector<std::size_t> SleepyTree::query(
        Clipper* clipper,
        const BBox& bbox,
        const std::size_t depthBegin,
        const std::size_t depthEnd)
{
    Roller roller(*m_bbox.get());
    std::vector<std::size_t> results;
    m_registry->query(roller, clipper, results, bbox, depthBegin, depthEnd);
    return results;
}

std::vector<char> SleepyTree::getPointData(
        Clipper* clipper,
        const std::size_t index,
        const Schema& reqSchema)
{
    std::vector<char> schemaPoint;
    std::vector<char> nativePoint(m_registry->getPointData(clipper, index));

    if (nativePoint.size())
    {
        schemaPoint.resize(reqSchema.pointSize());

        SinglePointTable table(schema(), nativePoint.data());
        LinkingPointView view(table);

        char* pos(schemaPoint.data());

        for (const auto& reqDim : reqSchema.dims())
        {
            view.getField(pos, reqDim.id(), reqDim.type(), 0);
            pos += reqDim.size();
        }
    }

    return schemaPoint;
}

const Schema& SleepyTree::schema() const
{
    return *m_schema.get();
}

std::size_t SleepyTree::numPoints() const
{
    return m_numPoints;
}

std::string SleepyTree::path() const
{
    return m_path;
}

std::string SleepyTree::name() const
{
    std::string name;

    // TODO Temporary/hacky.
    const std::size_t pos(m_path.find_last_of("/\\"));

    if (pos != std::string::npos)
    {
        name = m_path.substr(pos + 1);
    }
    else
    {
        name = m_path;
    }

    return name;
}

std::string SleepyTree::metaPath() const
{
    return m_path + "/meta";
}

Json::Value SleepyTree::getTreeMeta() const
{
    Json::Value jsonMeta;
    jsonMeta["bbox"] = m_bbox->toJson();
    jsonMeta["schema"] = m_schema->toJson();
    jsonMeta["dimensions"] = static_cast<Json::UInt64>(m_dimensions);
    jsonMeta["numPoints"] = static_cast<Json::UInt64>(m_numPoints);
    jsonMeta["numTossed"] = static_cast<Json::UInt64>(m_numTossed);

    // Add origin list to meta.
    Json::Value& jsonManifest(jsonMeta["manifest"]);
    for (Json::ArrayIndex i(0); i < m_originList.size(); ++i)
    {
        jsonManifest.append(m_originList[i]);
    }

    return jsonMeta;
}

Origin SleepyTree::addOrigin(const std::string& remote)
{
    const Origin origin(m_originList.size());
    m_originList.push_back(remote);
    return origin;
}

std::string SleepyTree::inferDriver(const std::string& remote) const
{
    const std::string driver(m_stageFactory->inferReaderDriver(remote));

    if (!driver.size())
    {
        throw std::runtime_error("No driver found - " + remote);
    }

    return driver;
}

std::string SleepyTree::fetchAndWriteFile(
        const std::string& remote,
        const Origin origin)
{
    // Fetch remote file and write locally.
    const std::string localPath("./tmp/" + name() + "-" +
                std::to_string(origin));

    std::size_t tries(0);
    HttpResponse res;

    do
    {
        res = m_s3->get(remote);
    } while (res.code() != 200 && ++tries < httpAttempts);

    if (res.code() != 200)
    {
        std::cout << "Couldn't fetch " + remote <<
                " - Got: " << res.code() << std::endl;
        throw std::runtime_error("Couldn't fetch " + remote);
    }

    if (!fs::writeFile(
                localPath,
                *res.data(),
                fs::binaryTruncMode))
    {
        throw std::runtime_error("Couldn't write " + localPath);
    }

    return localPath;
}

} // namespace entwine

