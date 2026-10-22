#include "script/ScriptRuntime.h"
#include <cstdio>
#include <fstream>
#include <string_view>
#include <limits>

namespace {
/// Fails in all build configurations.
void require(bool ok,const std::string& message) { if(!ok) throw std::runtime_error(message); }
/// Writes a project-relative script fixture.
void write(const std::filesystem::path& root,const char* name,const char* source) {
    // Scripts lie in the assets folder, which script paths start from.
    const bool script=std::string_view(name).size()>3 && std::string_view(name).substr(std::string_view(name).size()-3)==".lh";
    std::filesystem::create_directories(root/"assets");
    std::ofstream(script ? root/"assets"/name : root/name,std::ios::binary)<<source;
}
/// An isolated world with mutable expanded attachments and captured script output.
struct Session {
    throne::World world;
    throne::LevelExpansion expanded;
    std::vector<std::string> log;
    std::unique_ptr<throne::ScriptRuntime> runtime;
    /// Instantiates the authored world without running callbacks.
    void open(const std::filesystem::path& root) {
        world.level.addScene("scene.lton");
        throne::Library library(root); expanded=throne::expand(world,library);
        runtime=std::make_unique<throne::ScriptRuntime>(world,expanded,root,[this](const auto& text) { log.push_back(text); },throne::PlayScope::Level);
    }
    /// Starts a frame and returns only that frame's callback log.
    const std::vector<std::string>& frame() { log.clear(); runtime->update(static_cast<float>(throne::fixedStep)); return log; }
    /// Mutates attachments through the same API a game uses.
    void eval(const std::string& text) {
        const auto answer=runtime->evaluate("import^ throne\n"+text);
        require(answer.text.empty(),answer.text);
    }
};
}

int main() {
    const auto root=std::filesystem::temp_directory_path()/"throne_behaviour_system_smoke";
    std::filesystem::create_directories(root);
    try {
        write(root,"all.lh",R"(module^ test.all
import^ throne
public^let^ Behaviour=def^{
 self^{ priority=4.0, updatePriority=-1.5, lateupdatePriority=8.0 },
 update=p^self^,node:throne.SceneNode,dt:number^ { throne.log("U"..node.name()) },
 lateupdate=p^self^,node:throne.SceneNode,dt:number^ { throne.log("L"..node.name()) },
 onendofframe=p^self^,node:throne.SceneNode,dt:number^ { throne.log("E"..node.name()) },
}
)");
        write(root,"late.lh",R"(module^ test.late
import^ throne
public^let^ Behaviour=def^{
 lateupdate=p^self^,node:throne.SceneNode,dt:number^ { throne.log("L"..node.name()) },
}
)");
        const auto info=throne::ScriptRuntime::inspectBehaviour(root,"all.lh");
        require(info.methods==std::array<bool,3>{true,true,true} && info.defaults.priority==4 &&
                info.defaults.phasePriorities[0]==-1.5 && info.defaults.phasePriorities[1]==8,
                "Script callback/default reflection disagrees with its definition");
        Session order;
        auto& scene=order.world.scenes["scene.lton"];
        scene.addNode("A").behaviour.push_back({"all.lh",{}});
        scene.addNode("B").behaviour.push_back({"all.lh",{}});
        scene.addNode("C").behaviour.push_back({"all.lh",{}});
        scene.addNode("D").behaviour.push_back({"late.lh",{}});
        scene.addNode("Disabled").behaviour.push_back({"all.lh",{}});
        auto& a=scene.nodes[0].behaviour[0]; a.priority=3.25; a.phasePriorities[1]=-10;
        auto& c=scene.nodes[2].behaviour[0]; c.priority=-2.5; c.phasePriorities[0]=9;
        scene.nodes[4].behaviour[0].enabled=false;
        scene.save(root/"roundtrip.lton");
        const auto saved=throne::Scene::load(root/"roundtrip.lton");
        {
            using namespace throne::lton;
            Document document(root/"roundtrip.lton");
            const auto& nodes=table(field(document.root(),"nodes"));
            const auto& node=table(at(nodes,0));
            require(lhat_is_nil(field(node,"behaviours")) && lhat_is_nil(field(node,"behaviour")),
                    "Node still serializes a separate Behaviour list outside components");
            const auto& component=table(field(table(field(node,"components")),"behaviour"));
            const auto& attachment=table(at(component,0));
            require(sequence(component)==1 && lhat_is_object_kind(field(attachment,"data"),LHAT_OBJECT_TABLE) &&
                    lhat_is_nil(field(attachment,"fields")),"Behaviour did not serialize script instance data in its component");
        }
        require(saved.nodes[0].behaviour[0].id==a.id && saved.nodes[0].behaviour[0].priority==3.25 &&
                saved.nodes[0].behaviour[0].phasePriorities[1]==-10 && !saved.nodes[4].behaviour[0].enabled,
                "Priority, enabled state or attachment identity lost during serialization");
        order.open(root);
        require(order.frame()==std::vector<std::string>{"UB","UA","UC","LA","LC","LD","LB","EC","EA","EB"},
                "Method query, effective priority, or default phase placement is wrong");
        const auto query=order.runtime->behaviourMatches("UpdateBehaviourSystem");
        require(query.size()==3 && query[0].priority==-1.5 && query[1].priority==3.25 && query[2].priority==9,
                "Published execution order differs from the resolved priority sort");
        order.eval(R"(let^ a=throne.find("A")
let^ phase=throne.system("UpdateBehaviourSystem")
if^ a!=nil^ and^ phase!=nil^ { a.setBehaviourPhasePriority(0,phase,-20) })");
        require(order.runtime->behaviourMatches("UpdateBehaviourSystem")[0].node==1,"Priority changed the active frame's query");
        require(order.frame()[0]=="UA","Priority change did not apply at the next frame boundary");
        order.eval(R"(let^ a2=throne.find("A")
let^ phase2=throne.system("UpdateBehaviourSystem")
if^ a2!=nil^ and^ phase2!=nil^ { a2.setBehaviourPhasePriority(0,phase2,nil^) a2.setBehaviourPriority(0,nil^) })");
        require(order.frame()[0]=="UA","Equal priorities did not preserve scene/hierarchy order");

        write(root,"coroutine.lh",R"(module^ test.coroutine
import^ throne
public^let^ Behaviour=def^{
 update=p^self^,node:throne.SceneNode,dt:number^ {
  throne.log("begin")
  do^{
   let^ late=throne.system("LateUpdateBehaviourSystem")
   if^ late!=nil^ { yield^ late }
   let^ context=throne.currentSystem()
   if^ context!=nil^ { throne.log(context.name()) }
   let^ finish=throne.system("EndOfFrameBehaviourSystem")
   if^ finish!=nil^ { yield^ finish }
   throne.log("after end")
   let^ update=throne.system("UpdateBehaviourSystem")
   if^ update!=nil^ { yield^ update }
   throne.log("next update")
  finally^:
   throne.log("disposed")
  }
 },
 lateupdate=p^self^,node:throne.SceneNode,dt:number^ { throne.log("late method") },
 onendofframe=p^self^,node:throne.SceneNode,dt:number^ { throne.log("end method") },
}
)");
        Session coroutine;
        coroutine.world.scenes["scene.lton"].addNode("Coroutine").behaviour.push_back({"coroutine.lh",{}});
        coroutine.open(root);
        const std::vector<std::string> first={"begin","LateUpdateBehaviourSystem","late method","after end","end method"};
        require(coroutine.frame()==first,"Coroutine did not visit later phases in the same frame, or blocked independent methods");
        require(coroutine.frame()==std::vector<std::string>{"next update","disposed","late method","end method"},
                "Earlier-phase wait, finalization or non-reentrant method handling is wrong");
        require(coroutine.frame()==first,"A completed coroutine callback was not invoked again on the following frame");
        coroutine.eval(R"(let^ sleeping=throne.find("Coroutine")
if^ sleeping!=nil^ { sleeping.setBehaviourEnabled(0,false^) })");
        require(coroutine.frame().empty(),"Disabled Behaviour resumed or called a method");
        coroutine.eval(R"(let^ awake=throne.find("Coroutine")
if^ awake!=nil^ { awake.setBehaviourEnabled(0,true^) })");
        require(coroutine.frame()==std::vector<std::string>{"next update","disposed","late method","end method"},
                "Re-enabled Behaviour restarted its suspended invocation");
        require(coroutine.frame()==first,"Coroutine fixture failed to suspend before removal");
        coroutine.eval(R"(let^ removed=throne.find("Coroutine")
if^ removed!=nil^ { removed.removeBehaviour(0) })");
        require(coroutine.frame()==std::vector<std::string>{"disposed"},"Removing an attachment failed to dispose its coroutine");
        require(coroutine.runtime->behaviourMatches("UpdateBehaviourSystem").empty(),"Removed Behaviour remained in a query");
        coroutine.eval(R"(let^ added=throne.find("Coroutine")
if^ added!=nil^ { let^ index=added.addBehaviour("late.lh") })");
        require(coroutine.frame()==std::vector<std::string>{"LCoroutine"},"Runtime attachment of a new script did not update the method queries");

        write(root,"nil.lh",R"(module^ test.nilwait
import^ throne
public^let^ Behaviour=def^{ update=p^self^,node:throne.SceneNode,dt:number^ {
 throne.log("before") yield^ nil^ throne.log("after")
} }
)");
        Session nil;
        nil.world.scenes["scene.lton"].addNode("Nil").behaviour.push_back({"nil.lh",{}});
        nil.open(root);
        require(nil.frame()==std::vector<std::string>{"before"} && nil.frame()==std::vector<std::string>{"after"},
                "A same-system wait resumed twice in one frame or re-entered its method");
        write(root,"function.lh",R"(module^ test.function
import^ throne
public^let^ Behaviour=def^{
 update=f^self^,node:throne.SceneNode,dt:number^ -> number^ { return^ dt },
 lateupdate=p^self^,node:throne.SceneNode,dt:number^ -> number^ { throne.log("procedure result") return^ 7 },
 onendofframe=f^self^,node:throne.SceneNode,dt:number^ -> nil^ { return^ nil^ },
}
)");
        Session functions;
        functions.world.scenes["scene.lton"].addNode("Functions").behaviour.push_back({"function.lh",{}});
        functions.open(root);
        require(functions.frame()==std::vector<std::string>{"procedure result"},
                "Ordinary functions or procedures returning a value were rejected or treated as coroutines");
        write(root,"start.lh",R"(module^ test.start
import^ throne
public^let^ Behaviour=def^{ start=p^self^,node:throne.SceneNode { throne.log("start only") } }
)");
        Session starts;
        starts.world.scenes["scene.lton"].addNode("Start").behaviour.push_back({"start.lh",{}});
        starts.open(root);
        require(starts.frame()==std::vector<std::string>{"start only"} && starts.frame().empty(),
                "A start-only Behaviour was skipped or started more than once");
        Session hot;
        auto& hotNode=hot.world.scenes["scene.lton"].addNode("Hot");
        hotNode.behaviour.push_back({"coroutine.lh",{}});
        hot.open(root);
        require(hot.frame()==first,"Hot-add fixture did not suspend");
        hot.eval(R"(let^ host=throne.find("Hot")
if^ host!=nil^ { let^ index=host.addBehaviour("late.lh") })");
        require(hot.frame()==std::vector<std::string>{"next update","disposed","late method","LHot","end method"},
                "Installing an unknown script invalidated a suspended invocation");
        hot.frame();
        hot.log.clear(); hot.runtime.reset();
        require(hot.log==std::vector<std::string>{"disposed"},"Stopping Play did not finalize a suspended callback exactly once");
        Session shifted;
        auto& pair=shifted.world.scenes["scene.lton"].addNode("Pair");
        pair.behaviour.push_back({"nil.lh",{}}); pair.behaviour.push_back({"nil.lh",{}});
        shifted.open(root);
        require(shifted.frame()==std::vector<std::string>{"before","before"},"Attachments shared a coroutine instance");
        shifted.eval(R"(let^ pair=throne.find("Pair")
if^ pair!=nil^ { pair.removeBehaviour(0) })");
        require(shifted.frame()==std::vector<std::string>{"after"},"Removing the first attachment restarted the surviving attachment");
        throne::Prefab prefab;
        const auto prefabRoot=prefab.addNode("Root").id;
        prefab.addNode("Child",prefabRoot).behaviour.push_back({"nil.lh",{}});
        prefab.save(root/"shared.prefab.lton");
        Session prefabs;
        auto& prefabScene=prefabs.world.scenes["scene.lton"];
        prefabScene.addNode("First").prefab="shared.prefab.lton";
        prefabScene.addNode("Second").prefab="shared.prefab.lton";
        prefabs.open(root);
        require(prefabs.frame()==std::vector<std::string>{"before","before"},"Prefab fixture did not create independent instances");
        prefabs.expanded.instances[0].expansion.nodes[1].node.behaviour[0].priority=-3;
        prefabs.runtime->refreshBehaviours();
        require(prefabs.frame()==std::vector<std::string>{"after","after"},
                "A registry refresh merged equal node/attachment IDs from different prefab instances");
        write(root,"pure.lh",R"(module^ test.pure
import^ throne
public^let^ Behaviour=def^{ update=f^self^,node:throne.SceneNode,dt:number^ { yield^ nil^ } }
)");
        Session pure;
        pure.world.scenes["scene.lton"].addNode("Pure").behaviour.push_back({"pure.lh",{}});
        pure.open(root); pure.frame(); pure.runtime.reset();
        require(pure.log.empty(),"A pure coroutine could not be finalized");
        write(root,"controller.lh",R"(module^ test.controller
import^ throne
public^let^ Behaviour=def^{ update=p^self^,node:throne.SceneNode,dt:number^ {
 let^ target=throne.find("Target")
 if^ target!=nil^ { target.setBehaviourEnabled(0,false^) }
} }
)");
        Session mutation;
        auto& mutationScene=mutation.world.scenes["scene.lton"];
        mutationScene.addNode("Controller").behaviour.push_back({"controller.lh",{}});
        mutationScene.addNode("Target").behaviour.push_back({"late.lh",{}});
        mutation.open(root);
        require(mutation.frame()==std::vector<std::string>{"LTarget"} && mutation.frame().empty(),
                "A callback mutation changed the current frame instead of the next frame");
        write(root,"data.lh",R"(module^ test.data
import^ throne
public^let^ Behaviour=def^{
 self^{ @export degreesPerSecond=90 },
 update=p^self^,node:throne.SceneNode,dt:number^ {
  let^ t=node.transform()
  if^ t!=nil^ { t.setRotation(t.rotation()+throne.vec3(0,self^.degreesPerSecond*dt,0)) }
 },
}
)");
        write(root,"data.scene.lton",R"(format="ogrethrone-scene",version=2,
nodes={ {id=1,name="Data",parent=0,components={
 transform={position={0,0,0},rotation={0,0,0},scale={1,1,1}},
 behaviour={
  {script="data.lh",data={degreesPerSecond=45}},
  {script="data.lh",data={degreesPerSecond=90}},
 },
}} },
)");
        Session data;
        data.world.scenes["scene.lton"]=throne::Scene::load(root/"data.scene.lton");
        data.world.scenes["scene.lton"].save(root/"data-again.scene.lton");
        data.world.scenes["scene.lton"]=throne::Scene::load(root/"data-again.scene.lton");
        data.open(root); data.frame();
        require(std::abs(data.expanded.instances[0].expansion.nodes[0].node.transform->rotation.y-2.25f)<.001f,
                "Repeated scripts did not own independent data after loading components.behaviour");
        write(root,"obsolete.scene.lton",R"(format="ogrethrone-scene",version=2,
nodes={ {id=1,name="Obsolete",parent=0,behaviours={{script="data.lh"}}} },
)");
        bool rejected=false;
        try { throne::Scene::load(root/"obsolete.scene.lton"); }
        catch(const throne::lton::Invalid&) { rejected=true; }
        require(rejected,"The abolished node-level behaviours format was silently accepted");

        // RenderEvent attachments read the render snapshot before and after drawing, and cannot write it.
        write(root,"event.lh",R"(module^ test.event
import^ throne
public^let^ RenderEvent=def^{
 self^{ @export label="seen" },
 prerender=p^self^,node:throne.SceneNode,dt:number^ {
  let^ t=node.transform()
  if^ t!=nil^ { print(self^.label, t.position().x) }
 },
 postrender=p^self^,node:throne.SceneNode,dt:number^ {
  let^ t=node.transform()
  if^ t!=nil^ { t.setPosition(throne.vec3(9,9,9)) }
 },
}
)");
        write(root,"event.scene.lton",R"(format="ogrethrone-scene",version=2,
nodes={ {id=1,name="Event",parent=0,components={
 transform={position={2,0,0},rotation={0,0,0},scale={1,1,1}},
 renderEvent={ {script="event.lh",data={label="at"}} },
}} },
)");
        Session event;
        event.world.scenes["scene.lton"]=throne::Scene::load(root/"event.scene.lton");
        event.world.scenes["scene.lton"].save(root/"event-again.scene.lton");
        event.world.scenes["scene.lton"]=throne::Scene::load(root/"event-again.scene.lton");
        require(event.world.scenes["scene.lton"].nodes[0].renderEvent.size()==1,"components.renderEvent did not round-trip");
        require(throne::ScriptRuntime::fields(root,"event.lh","RenderEvent").size()==1,"RenderEvent field reflection failed");
        event.open(root);
        event.frame();
        require(event.log.empty(),"The simulation column ran a RenderEvent");
        throne::ScriptRuntime render(event.world,event.expanded,root,[&](const std::string& text) { event.log.push_back(text); },
                                     throne::PlayScope::RenderEvents);
        render.renderEvent(throne::renderStages[0],.5f);
        require(event.log.size()==1 && event.log[0].rfind("at 2",0)==0,"prerender did not read its node: "+(event.log.empty()?std::string():event.log[0]));
        bool refused=false;
        try { render.renderEvent(throne::renderStages[2],.5f); }
        catch(const throne::ScriptError& e) { refused=std::string(e.what()).find("read-only")!=std::string::npos; }
        require(refused && event.expanded.instances[0].expansion.nodes[0].node.transform->position.x==2,
                "postrender changed the render snapshot");
        std::filesystem::remove_all(root);
        std::puts("BehaviourSystem smoke passed: method queries, priority inheritance, stable ordering, frame-boundary edits, coroutine phases and attachment lifecycle");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
