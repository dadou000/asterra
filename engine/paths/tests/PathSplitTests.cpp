#include <orbit/paths/PathNetwork.hpp>
#include <orbit/paths/PathEvaluation.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <cmath>
#include <filesystem>
#include <iostream>

#define CHECK(x) do {if(!(x)){std::cerr << "PathSplit failed " #x " line " << __LINE__ << '\n';return 1;}}while(false)
int main()
{
    using namespace orbit;
    const auto root=std::filesystem::temp_directory_path() /
        ("orbit-split-" + documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);
    {
        auto project=documents::ProjectDocument::Create(root,"Path Split Test");
        documents::WorldDatabase world(project.StartupWorldPath());
        schema::SchemaRegistry schemas;
        paths::RegisterSchemas(schemas);
        scene::ObjectStore objects(world);
        commands::CommandService commands(objects,schemas);
        paths::PathNetworkService service(objects,commands);
        frames::FrameGraph frames;
        const auto frame=frames.CreateRoot();
        const auto network=service.CreateNetwork("Mixed");
        const math::Double3 a{0,0,0},b{90,0,0};
        auto n0=service.CreateNode(network.id,"A",
            paths::FramePointAnchor{frame,a});
        auto n1=service.CreateNode(network.id,"B",
            paths::FramePointAnchor{frame,b});
        const math::Double3 h1{20,1,35},h2{-20,-1,35};
        auto edge=service.ConnectBezier(n0.id,n1.id,h1,h2,"Curve");
        service.SetInfrastructureIntent(edge.id,"ballasted-rail",1,"bridge",
            85.0,true);
        const double fraction=0.37;
        auto split=service.SplitEdge(edge.id,fraction,"Switch node");
        CHECK(split.firstEdge.id==edge.id);
        CHECK(split.firstEdge.endNode==split.junctionNode.id);
        CHECK(split.secondEdge.startNode==split.junctionNode.id);
        CHECK(split.secondEdge.endNode==n1.id);
        CHECK(split.secondEdge.infrastructureCatalogId=="ballasted-rail");
        CHECK(split.secondEdge.constructionMode=="bridge");
        CHECK(split.secondEdge.designSpeedKmh==85.0);
        const auto at=std::get<paths::FramePointAnchor>(split.junctionNode.anchor);
        CHECK(at.frame==frame);
        for(int i=0;i<=100;++i)
        {
            const double t=static_cast<double>(i)/100.0;
            const auto original=paths::EvaluateBezier(a,b,h1,h2,t);
            math::Double3 reconstruction;
            if(t<=fraction)
            {
                const auto q=static_cast<double>(t/fraction);
                reconstruction=paths::EvaluateBezier(a,at.localMeters,
                    split.firstEdge.startHandleMeters,
                    split.firstEdge.endHandleMeters,q);
            }
            else
            {
                const auto q=(t-fraction)/(1.0-fraction);
                reconstruction=paths::EvaluateBezier(at.localMeters,b,
                    split.secondEdge.startHandleMeters,
                    split.secondEdge.endHandleMeters,q);
            }
            CHECK(math::Length(original-reconstruction)<1.e-8);
        }
        commands.Undo();
        CHECK(!service.FindNode(split.junctionNode.id));
        CHECK(!service.FindEdge(split.secondEdge.id));
        CHECK(service.FindEdge(edge.id)->endNode==n1.id);
        CHECK(service.FindEdge(edge.id)->startHandleMeters==h1);
        CHECK(service.FindEdge(edge.id)->endHandleMeters==h2);
        commands.Redo();
        CHECK(service.FindNode(split.junctionNode.id).has_value());
        CHECK(service.FindEdge(split.secondEdge.id).has_value());
        CHECK(service.FindEdge(edge.id)->endNode==split.junctionNode.id);
        auto straight=service.ConnectDirect(n0.id,n1.id,"Straight");
        const auto ds=service.SplitEdge(straight.id,0.25);
        CHECK(ds.firstEdge.mode==paths::EdgeMode::Direct);
        CHECK(ds.secondEdge.mode==paths::EdgeMode::Direct);
        CHECK(std::abs(std::get<paths::FramePointAnchor>(
            ds.junctionNode.anchor).localMeters.x-22.5)<1.e-9);
        bool rejected=false;
        try{static_cast<void>(service.SplitEdge(straight.id,1.0));}
        catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected);
        const auto n2=service.CreateNode(network.id,"Other",
            paths::FramePointAnchor{frames.CreateFrame(frame,
                [](time::SimulationTime){return math::RigidTransformD{};}),{5,0,0}});
        auto mixed=service.ConnectDirect(n0.id,n2.id);
        rejected=false;
        try{static_cast<void>(service.SplitEdge(mixed.id,0.5));}
        catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected);
        CHECK(service.FindEdge(mixed.id)->endNode==n2.id);
        auto routed=service.ConnectRouted(n0.id,n1.id);
        rejected=false;
        try{static_cast<void>(service.SplitEdge(routed.id,0.5));}
        catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected);
    }
    std::filesystem::remove_all(root);
    return 0;
}
