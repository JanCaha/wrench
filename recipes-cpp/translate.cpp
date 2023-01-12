
#include <iostream>
#include <filesystem>
#include <thread>

#include <pdal/PipelineManager.hpp>
#include <pdal/Stage.hpp>
#include <pdal/util/ProgramArgs.hpp>
#include <pdal/pdal_types.hpp>
#include <pdal/Polygon.hpp>

#include <gdal/gdal_utils.h>

#include "utils.hpp"
#include "alg.hpp"
#include "vpc.hpp"

using namespace pdal;

namespace fs = std::filesystem;


void Translate::addArgs()
{
    argOutput = &programArgs.add("output,o", "Output point cloud file", outputFile);
    argOutputFormat = &programArgs.add("output-format", "Output format (las/laz/copc)", outputFormat);
    programArgs.add("assign-crs", "Assigns CRS to data (no reprojection)", assignCrs);
    programArgs.add("transform-crs", "Transforms (reprojects) data to anoter CRS", transformCrs);
}

bool Translate::checkArgs()
{
    if (!argOutput->set())
    {
        std::cerr << "missing output" << std::endl;
        return false;
    }

    // TODO: or use the same format as the reader?
    if (argOutputFormat->set())
    {
        if (outputFormat != "las" && outputFormat != "laz")
        {
            std::cerr << "unknown output format: " << outputFormat << std::endl;
            return false;
        }
    }
    else
        outputFormat = "las";  // uncompressed by default

    return true;
}


static std::unique_ptr<PipelineManager> pipeline(ParallelJobInfo *tile, std::string assignCrs, std::string transformCrs)
{
    std::unique_ptr<PipelineManager> manager( new PipelineManager );

    Options reader_opts;
    if (!assignCrs.empty())
        reader_opts.add(pdal::Option("override_srs", assignCrs));

    Stage& r = manager->makeReader( tile->inputFilenames[0], "", reader_opts);

    pdal::Options writer_opts;
    writer_opts.add(pdal::Option("forward", "all"));

    // optional reprojection
    Stage* reproject = nullptr;
    if (!transformCrs.empty())
    {
        Options transform_opts;
        transform_opts.add(pdal::Option("out_srs", transformCrs));
        reproject = &manager->makeFilter( "filters.reprojection", r, transform_opts);
    }

    Stage& writerInput = reproject ? *reproject : r;
    Stage& w = manager->makeWriter( tile->outputFilename, "", writerInput, writer_opts);

    if (!tile->filterExpression.empty())
    {
        Options filter_opts;
        filter_opts.add(pdal::Option("where", tile->filterExpression));
        w.addOptions(filter_opts);
        if (reproject)
            reproject->addOptions(filter_opts);
    }

    return manager;
}


void Translate::preparePipelines(std::vector<std::unique_ptr<PipelineManager>>& pipelines, const BOX3D &bounds, point_count_t &totalPoints)
{
    if (ends_with(inputFile, ".vpc"))
    {
        if (!ends_with(outputFile, ".vpc"))
        {
            std::cout << "output should be VPC too" << std::endl;
            return;
        }

        // for /tmp/hello.vpc we will use /tmp/hello dir for all results
        fs::path outputParentDir = fs::path(outputFile).parent_path();
        fs::path outputSubdir = outputParentDir / fs::path(outputFile).stem();
        fs::create_directories(outputSubdir);

        // VPC handling
        VirtualPointCloud vpc;
        if (!vpc.read(inputFile))
            return;

        for (const VirtualPointCloud::File& f : vpc.files)
        {
            ParallelJobInfo tile(ParallelJobInfo::FileBased, BOX2D(), filterExpression);
            tile.inputFilenames.push_back(f.filename);

            // for input file /x/y/z.las that goes to /tmp/hello.vpc,
            // individual output file will be called /tmp/hello/z.las
            fs::path inputBasename = fs::path(f.filename).stem();
            tile.outputFilename = (outputSubdir / inputBasename).string() + "." + outputFormat;

            tileOutputFiles.push_back(tile.outputFilename);

            pipelines.push_back(pipeline(&tile, assignCrs, transformCrs));
        }
    }
    else
    {
        ParallelJobInfo tile(ParallelJobInfo::Single, BOX2D(), filterExpression);
        tile.inputFilenames.push_back(inputFile);
        tile.outputFilename = outputFile;
        pipelines.push_back(pipeline(&tile, assignCrs, transformCrs));
    }
}

void Translate::finalize(std::vector<std::unique_ptr<PipelineManager>>& pipelines)
{
    if (tileOutputFiles.empty())
        return;

    // now build a new output VPC
    std::vector<std::string> args;
    args.push_back("--output=" + outputFile);
    for (std::string f : tileOutputFiles)
        args.push_back(f);
    buildVpc(args);
}
