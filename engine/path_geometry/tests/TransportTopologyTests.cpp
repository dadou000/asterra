#include <orbit/path_geometry/TransportTopology.hpp>
#include <algorithm>
#include <iostream>
#include <vector>

#define CHECK(x) do { if (!(x)) {std::cerr << "FAIL " #x " at " << __LINE__ << '\n';return 1;} } while(false)

int main()
{
    using namespace orbit::path_geometry;
    using orbit::scene::ObjectId;
    const orbit::paths::NetworkId network{1,1}, other{2,2};
    const std::vector<TransportNode> nodes{
        {{1,1},network},{{1,2},network},{{1,3},network},
        {{1,4},network},{{1,5},other}
    };
    const std::vector<TransportLink> links{
        {{2,1},network,{1,1},{1,2},TransportMode::Road,true},
        {{2,2},network,{1,2},{1,3},TransportMode::Road,false},
        {{2,3},network,{1,2},{1,4},TransportMode::Rail,true}
    };
    const auto good=CompileTransportTopology(nodes,links);
    CHECK(good.Valid());
    CHECK(good.arcs.size()==5);
    CHECK(good.components.size()==2);
    CHECK(good.junctions.size()==1);
    CHECK(good.junctions[0].node==(ObjectId{1,2}));
    CHECK(good.junctions[0].explicitLevelCrossing);
    CHECK(good.junctions[0].roadLinks==2);
    CHECK(good.junctions[0].railLinks==1);
    CHECK(good.components[0].mode==TransportMode::Road);
    CHECK(good.components[0].nodes.size()==3);
    CHECK(good.components[1].mode==TransportMode::Rail);
    CHECK(good.components[1].nodes.size()==2);
    for (const auto& arc:good.arcs)
        CHECK(!(arc.link==(ObjectId{2,2}) && arc.to==(ObjectId{1,2})));
    auto reordered=links;
    std::reverse(reordered.begin(),reordered.end());
    const auto same=CompileTransportTopology(nodes,reordered);
    CHECK(same.arcs.size()==good.arcs.size());
    for (std::size_t i=0;i<same.arcs.size();++i)
        CHECK(same.arcs[i].link==good.arcs[i].link &&
              same.arcs[i].from==good.arcs[i].from);
    reordered.push_back({{2,4},network,{1,4},{1,5},TransportMode::Rail,true});
    reordered.push_back({{2,5},network,{1,3},{1,9},TransportMode::Road,true});
    reordered.push_back({{2,5},network,{1,1},{1,2},TransportMode::Road,true});
    const auto invalid=CompileTransportTopology(nodes,reordered);
    CHECK(!invalid.Valid());
    CHECK(invalid.findings.size()==3);
    return 0;
}
