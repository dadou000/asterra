#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

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
        const auto firstPublicationDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < firstPublicationDeadline)
        {
            static_cast<void>(studio.Tick(false));
            if (studio.PathProducts().Find(e.id) &&
                studio.PathProducts().Find(w.id) &&
                studio.PathProducts().Find(n.id) &&
                studio.RoadJunctions().ActiveJunctions() == 1U) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
        // No contact packet => no stepping old actors or silently resetting
        // their physics states while the road rebuild is in flight.
        {
            const rpc::Value step(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id","dirty-step"},
                {"method","infrastructure.vehicle_world"},
                {"params",rpc::Value::Object{
                    {"action","step"},{"node",centre.id.ToString()},
                    {"steps",static_cast<i64>(1)}}
            });
            const auto reply=studio.DispatchRpc(rpc::Serialize(step));
            CHECK(reply.has_value());
            const auto value=rpc::ParseValue(*reply);
            const auto* result=value.Find("result");
            CHECK(result && result->IsObject());
            const auto* ready=result->Find("ready");
            CHECK(ready && ready->IsBool() && !ready->AsBool());
            CHECK(studio.Vehicles().Tick()==60U);
        }

        CHECK(!pipeline.ProbeContact(centre.id,studio.PathProducts(),
            {.frame=frame,.origin={2,10,3}}).has_value());
        const auto recoveryDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < recoveryDeadline)
        {
            static_cast<void>(studio.Tick(false));
            if (pipeline.CurrentContact(centre.id, studio.PathProducts())) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(pipeline.CurrentContact(centre.id,studio.PathProducts())!=nullptr);


        // The same fixed-step, four-wheel seam fixture is reachable through
        // the existing Studio RPC dispatcher, without spawning a game actor.
        {
            const rpc::Value request(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id","drive"},
                {"method","infrastructure.road_vehicle_fixture"},
                {"params",rpc::Value::Object{
                    {"node",centre.id.ToString()},
                    {"steps",static_cast<i64>(480)}
                }}
            });
            const auto reply=studio.DispatchRpc(rpc::Serialize(request));
            CHECK(reply.has_value());
            const auto parsed=rpc::ParseValue(*reply);
            CHECK(parsed.Find("error")==nullptr);
            const auto* result=parsed.Find("result");
            CHECK(result && result->IsObject());
            const auto* ready=result->Find("ready");
            CHECK(ready && ready->IsBool() && ready->AsBool());
            const auto* complete=result->Find("completed");
            CHECK(complete && complete->IsBool() && complete->AsBool());
            const auto* steps=result->Find("steps");
            CHECK(steps && steps->IsInteger() && steps->AsInteger()==480);
            const auto* signature=result->Find("packet_signature");
            CHECK(signature && signature->IsString() &&
                  !signature->AsString().empty());
        }
        // Persistent runtime vehicles use the same generation-fenced
        // contact owner. They are transient (not scene DB objects).
        scene::ObjectId runtimeVehicle{};
        {
            const rpc::Value spawn(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id","spawn"},
                {"method","infrastructure.vehicle_world"},
                {"params",rpc::Value::Object{
                    {"action","spawn"},{"node",centre.id.ToString()}}
            });
            const auto reply=studio.DispatchRpc(rpc::Serialize(spawn));
            CHECK(reply.has_value());
            const auto parsed=rpc::ParseValue(*reply);
            CHECK(parsed.Find("error")==nullptr);
            const auto* data=parsed.Find("result");
            CHECK(data && data->IsObject());
            const auto* created=data->Find("vehicle");
            CHECK(created && created->IsString());
            const auto id=scene::ObjectId::Parse(created->AsString());
            CHECK(id.has_value());
            runtimeVehicle=*id;
            CHECK(studio.Vehicles().Find(runtimeVehicle)!=nullptr);
            const rpc::Value step(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id","step"},
                {"method","infrastructure.vehicle_world"},
                {"params",rpc::Value::Object{
                    {"action","step"},{"node",centre.id.ToString()},
                    {"steps",static_cast<i64>(60)}}
            });
            const auto stepped=studio.DispatchRpc(rpc::Serialize(step));
            CHECK(stepped.has_value());
            const auto stepValue=rpc::ParseValue(*stepped);
            CHECK(stepValue.Find("error")==nullptr);
            const auto* stepResult=stepValue.Find("result");
            CHECK(stepResult && stepResult->IsObject());
            const auto* ready=stepResult->Find("ready");
            CHECK(ready && ready->IsBool() && ready->AsBool());
            CHECK(studio.Vehicles().Tick()==60U);
        }
        const auto previous=pipeline.Revision();
        const auto beforeContact=pipeline.CurrentContact(
            centre.id,studio.PathProducts());
        CHECK(beforeContact!=nullptr);
        const auto beforeSignature=beforeContact->packetSignature;
        paths.SetNodeAnchor(east.id,paths::FramePointAnchor{
            .frame=frame,.localMeters={38,0,0}});
        const auto editedRoadDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < editedRoadDeadline)
        {
            static_cast<void>(studio.Tick(false));
            const auto* contact = studio.RoadJunctions().CurrentContact(
                centre.id, studio.PathProducts());
            if (contact && contact->packetSignature != beforeSignature) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(studio.RoadJunctions().Revision()>previous);
        CHECK(studio.RoadJunctions().FindPatch(centre.id)!=nullptr);
        const auto afterContact=studio.RoadJunctions().CurrentContact(
            centre.id,studio.PathProducts());
        CHECK(afterContact!=nullptr);
        CHECK(afterContact->packetSignature!=beforeSignature);

        // Build a scene much larger than one discovery tick. The prior
        // contact must be retired immediately, and the junction must be
        // republished only after all 96-record discovery slices complete.
        for (int i = 0; i < 150; ++i)
            static_cast<void>(paths.CreateNode(network.id,
                "Unused " + std::to_string(i),
                paths::FramePointAnchor{.frame=frame,
                    .localMeters={0.0, 0.0, 100.0 + i}}));
        static_cast<void>(studio.Tick(false));
        CHECK(studio.RoadJunctions().ActiveContactScenes()==0U);
        CHECK(studio.RoadJunctions().CurrentContact(
            centre.id,studio.PathProducts())==nullptr);
        const auto discoveryDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < discoveryDeadline)
        {
            static_cast<void>(studio.Tick(false));
            if (studio.RoadJunctions().CurrentContact(
                    centre.id,studio.PathProducts()) != nullptr)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(studio.RoadJunctions().CurrentContact(
            centre.id,studio.PathProducts())!=nullptr);
        // The previous temporary vehicle remains explicit but cannot step
        // using the new geometry until re-bound (which clears old actors).
        CHECK(studio.Vehicles().Find(runtimeVehicle)!=nullptr);
        studio.CloseWorld();
        CHECK(studio.RoadJunctions().FindPatch(centre.id)==nullptr);
        CHECK(studio.RoadJunctions().Packets().empty());
        CHECK(studio.RoadJunctions().ActiveContactScenes()==0U);
        CHECK(studio.Vehicles().Count()==0U);


    }
    std::filesystem::remove_all(root);
    return 0;
}
