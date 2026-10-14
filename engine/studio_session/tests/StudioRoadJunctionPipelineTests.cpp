#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#define CHECK(x) do {if(!(x)){std::cerr<<"StudioRoadJunctionPipeline: " #x " at "<<__LINE__<<'\n';return 1;}}while(false)

int main()
{
    using namespace orbit;
    const auto root=std::filesystem::temp_directory_path()/
        ("orbit-road-publication-"+documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);
    {
        auto project=documents::ProjectDocument::Create(root,"Road publication");
        studio_session::StudioSession studio(project);
        auto& world=studio.World();
        auto& commands=world.Commands();
        const auto worldObj=commands.CreateObject(
            world_model::kWorldType,"World");
        const auto system=commands.CreateObject(
            world_model::kCelestialSystemType,"Helion",worldObj);
        const auto body=commands.CreateObject(
            world_model::kCelestialBodyType,"Asterra",system);
        commands.SetProperty(body,world_model::kBodyRadius,6000000.0);
        CHECK(world.RefreshUniverseIfChanged());
        const auto bodyId=world.Universe().BodyForObject(body);
        CHECK(bodyId.has_value());
        const auto* bodyRecord=world.Universe().Bodies().FindBody(*bodyId);
        CHECK(bodyRecord!=nullptr);
        const auto frame=bodyRecord->frame;
        auto& paths=studio.PathNetwork().Service();
        const auto network=paths.CreateNetwork("Junction",worldObj);
        const auto centre=paths.CreateNode(network.id,"Centre",
            paths::FramePointAnchor{.frame=frame,.localMeters={0,0,0}});
        const auto east=paths.CreateNode(network.id,"East",
            paths::FramePointAnchor{.frame=frame,.localMeters={32,0,0}});
        const auto west=paths.CreateNode(network.id,"West",
            paths::FramePointAnchor{.frame=frame,.localMeters={-32,0,0}});
        const auto north=paths.CreateNode(network.id,"North",
            paths::FramePointAnchor{.frame=frame,.localMeters={0,0,32}});
        const auto e=paths.ConnectDirect(centre.id,east.id);
        const auto w=paths.ConnectDirect(centre.id,west.id);
        const auto n=paths.ConnectDirect(centre.id,north.id);
        for(const auto edge:{e.id,w.id,n.id})
            paths.SetInfrastructureIntent(
                edge,"rural-road",1,"terrain",60.0,true);
        for(int i=0;i<12;++i)static_cast<void>(studio.Tick(false));
        CHECK(studio.PathProducts().Find(e.id)!=nullptr);
        CHECK(studio.PathProducts().Find(w.id)!=nullptr);
        CHECK(studio.PathProducts().Find(n.id)!=nullptr);
        const auto& pipeline=studio.RoadJunctions();
        CHECK(pipeline.ActiveJunctions()==1U);
        CHECK(pipeline.FindPatch(centre.id)!=nullptr);
        CHECK(pipeline.FindEdge(e.id)!=nullptr);
        CHECK(pipeline.FindEdge(e.id)->product.visualMesh.indices.size()<
              studio.PathProducts().Find(e.id)->visualMesh.indices.size());
        CHECK(pipeline.Packets().size()==1U);
        const auto render=studio.RoadJunctions().RenderProducts(
            studio.PathProducts());
        CHECK(render.size()==4U); // Three corridors + one unique patch.
        bool sawPatch=false,sawTrimmedEast=false;
        for(const auto* p:render)
        {
            if(p->edge==centre.id) {
                sawPatch=true;
                CHECK(!p->visualMesh.indices.empty());
            }
            if(p->edge==e.id) {
                sawTrimmedEast=true;
                CHECK(p->visualMesh.indices.size()<
                      studio.PathProducts().Find(e.id)->visualMesh.indices.size());
            }
        }
        CHECK(sawPatch && sawTrimmedEast);
        CHECK(pipeline.ActiveContactScenes()==1U);
        CHECK(pipeline.CurrentContact(centre.id,studio.PathProducts())!=nullptr);
        CHECK(pipeline.ProbeContact(centre.id,studio.PathProducts(),
            {.frame=frame,.origin={2,10,3},.direction={0,-1,0}}).has_value());
        // Exercise the same read-only CPU contact RPC available to agents.
        {
            const rpc::Value request(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id","probe"},
                {"method","infrastructure.junction_contact_probe"},
                {"params",rpc::Value::Object{
                    {"node",centre.id.ToString()},
                    {"origin_frame_local_meters",
                        rpc::Value::Array{2.0,10.0,3.0}},
                    {"direction_frame_local",
                        rpc::Value::Array{0.0,-1.0,0.0}},
                    {"maximum_distance_meters",30.0}
                }}
            });
            const auto response=studio.DispatchRpc(rpc::Serialize(request));
            CHECK(response.has_value());
            const auto value=rpc::ParseValue(*response);
            CHECK(value.Find("error")==nullptr);
            const auto* result=value.Find("result");
            CHECK(result && result->IsObject());
            const auto* ready=result->Find("ready");
            CHECK(ready && ready->IsBool() && ready->AsBool());
            const auto* cpu=result->Find("cpu_only");
            CHECK(cpu && cpu->IsBool() && cpu->AsBool());
        }
        studio.PathProducts().MarkDirty(e.id);
        const auto dirtyRender=studio.RoadJunctions().RenderProducts(
            studio.PathProducts());
        CHECK(dirtyRender.size()==3U); // Suppress entire stale frame packet.
        CHECK(dirtyRender[0]->edge!=centre.id);
        CHECK(pipeline.CurrentContact(centre.id,studio.PathProducts())==nullptr);
        CHECK(!pipeline.ProbeContact(centre.id,studio.PathProducts(),
            {.frame=frame,.origin={2,10,3}}).has_value());
        for(int i=0;i<12;++i)static_cast<void>(studio.Tick(false));
        CHECK(pipeline.CurrentContact(centre.id,studio.PathProducts())!=nullptr);


        const auto previous=pipeline.Revision();
        paths.SetNodeAnchor(east.id,paths::FramePointAnchor{
            .frame=frame,.localMeters={38,0,0}});
        for(int i=0;i<12;++i)static_cast<void>(studio.Tick(false));
        CHECK(studio.RoadJunctions().Revision()>previous);
        CHECK(studio.RoadJunctions().FindPatch(centre.id)!=nullptr);
        studio.CloseWorld();
        CHECK(studio.RoadJunctions().FindPatch(centre.id)==nullptr);
        CHECK(studio.RoadJunctions().Packets().empty());
        CHECK(studio.RoadJunctions().ActiveContactScenes()==0U);

    }
    std::filesystem::remove_all(root);
    return 0;
}
