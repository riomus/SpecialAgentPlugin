# SpecialAgent

**Connect AI to Unreal Engine 5**

Full Python API access • ~294 tools across 45 services • Visual feedback loop

---

## What's new

- **Phase 0:** fixed a task-graph reentrancy crash that could hang the editor when MCP requests were dispatched to the game thread while another game-thread task was mid-flight. Requests are now dispatched without re-entering the calling task.
- **Phase 3:** expanded the service surface from 13 to 45 services (~294 tools), added 12 new MCP prompts, and refreshed the system instructions string sent to clients.

---

## What is SpecialAgent?

SpecialAgent bridges AI assistants and Unreal Engine 5 through the **Model Context Protocol (MCP)**. Connect Claude, GPT, or any MCP-compatible LLM directly to your editor and control it through natural language.

At its core, SpecialAgent provides **unrestricted Python execution** with full access to UE5's Python API—meaning your AI assistant can do anything the editor can do. On top of that foundation, **~294 purpose-built tools across 45 services** handle common level design, content, rendering, gameplay, and automation tasks without writing a single line of code.

Native HTTP/SSE transport. No external bridges or dependencies.

---

## Features

### Two Layers of Power

#### Full Python Access

Execute arbitrary Python with complete `unreal` module access. Your AI assistant can:

- Import and process assets
- Create and modify Blueprints  
- Generate materials and textures
- Automate project configuration
- Build custom editor utilities
- Run validation and QA checks
- Anything the UE5 Python API supports

This is the unlimited foundation. If you can script it, AI can do it.

#### Level Design & Editor Toolkit

~294 specialized tools across 45 services for world-building, content, gameplay and automation workflows:

| Category | Capabilities |
|----------|-------------|
| **Actors** | Spawn, transform, duplicate, delete, batch operations |
| **Patterns** | Grid, circular, spline, and scatter placement |
| **Landscape** | Sculpt height, flatten, smooth, paint material layers |
| **Foliage** | Paint vegetation with density control |
| **Lighting / Sky** | Lights, sky atmosphere, fog, clouds, build lightmaps |
| **Streaming / WP** | Manage sub-levels, World Partition cell loading |
| **Navigation** | Rebuild NavMesh, test pathfinding |
| **Performance** | Statistics, overlap detection, triangle counts |
| **Blueprints** | Create, compile, edit, spawn, introspect |
| **Materials** | Materials + instances + parameter editing |
| **Components / Physics / Anim / AI** | Actor components, physics, skeletal anim, AI pawns/BT |
| **Post-Process / Decals / Niagara / Sound** | Visuals and audio |
| **Sequencer / Render Queue** | Cinematics and movie render output |
| **PIE / Console / Log / Level** | Runtime control, CVars, logs, level ops |
| **Asset Import / Content Browser / Validation / Deps / Data Tables** | Content pipeline |
| **Reflection / Project / Source Control** | Introspection, project settings, SCM |
| **PCG / Modeling / HLOD / Rendering / Editor Mode** | Procedural content, modeling tools, HLOD, scalability |
| **Organization** | Folders, tags, labels, selection management |

### Visual Feedback Loop

Capture viewport screenshots and return them to vision-enabled LLMs. Your AI assistant can see what it built, evaluate the results, and refine its approach.

```
Describe intent → Execute → Screenshot → AI analyzes → Iterate
```

---

## Installation

### Requirements

- Unreal Engine 5.6 or later
- Windows, Mac, or Linux
- MCP-compatible client (Cursor, Claude Desktop, etc.)

### Setup

1. **Clone or download** this repository into your project's `Plugins` folder:
   ```
   YourProject/
   └── Plugins/
       └── SpecialAgent/
   ```

2. **Regenerate project files** (right-click `.uproject` → Generate Visual Studio/Xcode project files)

3. **Build and launch** your project

4. **Enable the plugin** in Edit → Plugins → Search "SpecialAgent"

5. **Restart** the editor

---

## Quick Start

### 1. Verify the Server

Once the editor launches, check the Output Log for:

```
LogSpecialAgent: MCP Server started on port 8767
```

Or test with curl:

```bash
curl http://localhost:8767/health
```

### 2. Configure Your MCP Client

Add SpecialAgent to your Codex MCP configuration:

```json
{
  "mcpServers": {
    "SpecialAgent": {
      "url": "http://localhost:8767/codex"
    }
  }
}
```

### 3. Connect and Build

Your AI assistant now has access to:
- Python execution with full UE5 API
- ~294 tools across 45 services
- Viewport screenshot capture
- Editor utilities (save, undo, redo)

---

## Service Categories

Counts are approximate as services evolve; call `tools/list` at runtime for the authoritative list.

| Service | Tools | Description |
|---------|:-----:|-------------|
| **python** | 3 | Execute scripts, run files, list modules |
| **screenshot** | 2 | Capture viewport for AI vision |
| **world** | 35 | Actor spawn/transform/delete, patterns, spatial queries |
| **lighting** | 6 | Light spawning, configuration, build lighting |
| **foliage** | 5 | Procedural foliage painting and removal |
| **landscape** | 6 | Terrain sculpting and layer painting |
| **streaming** | 5 | Sub-level loading and visibility |
| **navigation** | 4 | NavMesh building and path testing |
| **world_partition** | 5 | World Partition cell loading |
| **gameplay** | 6 | Trigger volumes, player starts, kill volumes |
| **performance** | 5 | Statistics, overlap analysis, triangle counts |
| **assets** | 16 | Asset Registry search, metadata, bounds |
| **content_browser** | 9 | Content Browser UI operations |
| **asset_import** | 6 | FBX/texture/sound/CSV import |
| **asset_deps** | 4 | Asset references/referencers |
| **data_table** | 7 | Read/write DataTable rows |
| **validation** | 3 | Asset and level validation |
| **blueprint** | 10 | Blueprint create/compile/edit |
| **material** | 8 | Materials, instances, parameter editing |
| **reflection** | 5 | UClass/UProperty/UFunction introspection |
| **component** | 7 | Actor component manipulation |
| **physics** | 7 | Physics simulation controls |
| **animation** | 5 | Skeletal animation |
| **ai** | 5 | AI pawns, behavior trees, blackboards |
| **input** | 4 | Input mappings |
| **sound** | 4 | Sound playback |
| **post_process** | 6 | Post-process volumes |
| **sky** | 5 | Sky atmosphere, fog, clouds, sky light |
| **decal** | 3 | Decal actors |
| **sequencer** | 6 | Level Sequence authoring |
| **niagara** | 6 | Niagara VFX |
| **render_queue** | 3 | Movie Render Queue |
| **rendering** | 5 | Scalability, view modes, screenshots |
| **pie** | 8 | Play In Editor control |
| **console** | 4 | Console commands and CVars |
| **log** | 4 | Log tail and categories |
| **level** | 5 | Level open/new/save |
| **editor_mode** | 3 | Landscape/foliage/modeling mode |
| **project** | 8 | Project settings and plugins |
| **source_control** | 5 | Source control operations |
| **pcg** | 3 | PCG graphs |
| **modeling** | 4 | Mesh booleans/extrude/simplify |
| **hlod** | 3 | Hierarchical LOD |
| **utility** | 18 | Save, undo, selection, transactions |
| **viewport** | 13 | Camera, view modes, bookmarks |

---

## Example Workflows

### Populate a Forest (via Tools)

```
1. assets/search → Find tree and rock assets
2. world/scatter_in_area → Place 500 trees with randomization
3. foliage/paint_in_area → Add grass and ground cover
4. screenshot/capture → Get visual for AI analysis
5. Iterate based on feedback
```

---

## Configuration

Edit `Config/DefaultSpecialAgent.ini` to customize:

```ini
[/Script/SpecialAgent.SpecialAgentSettings]
; Server port (change if 8767 is in use)
ServerPort=8767

; Auto-start server when editor launches
bAutoStart=true

; Enable verbose logging
bVerboseLogging=false
```

---

## Architecture

```
┌─────────────────────────────────────────┐
│        MCP Client (Claude, etc.)        │
└──────────────┬──────────────────────────┘
               │ HTTP/SSE + JSON-RPC 2.0
┌──────────────▼──────────────────────────┐
│       SpecialAgent MCP Server           │
│                                         │
│  ┌─────────────────────────────────┐    │
│  │   Python Service (Primary)      │    │
│  │   Full unreal module access     │    │
│  └─────────────────────────────────┘    │
│                                         │
│  ┌─────────────────────────────────┐    │
│  │   45 Services (~294 Tools)      │    │
│  │   Level design, content,        │    │
│  │   gameplay, rendering, more     │    │
│  └─────────────────────────────────┘    │
│                                         │
│  ┌─────────────────────────────────┐    │
│  │   Game Thread Dispatcher        │    │
│  │   Thread-safe API access        │    │
│  └─────────────────────────────────┘    │
└──────────────┬──────────────────────────┘
               │
┌──────────────▼──────────────────────────┐
│        Unreal Engine 5 Editor           │
└─────────────────────────────────────────┘
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [QUICKSTART.md](QUICKSTART.md) | Step-by-step setup guide |
| [STRUCTURE.md](STRUCTURE.md) | Plugin architecture and file layout |

---

## Design Philosophy

The ~294 tools exist for convenience and discoverability. Python execution is the real power.

When your AI assistant sees `world/place_in_circle`, it learns circular placement is possible. But for custom logic—density falloff, terrain-aware positioning, asset variation based on rules—it writes Python.

Both layers work together: quick tools for common tasks, unlimited scripting for everything else.

---

## Troubleshooting

### Server Won't Start

- Check if port 8767 is in use: `netstat -an | grep 8767`
- Change port in `DefaultSpecialAgent.ini`
- Verify plugin is enabled in Edit → Plugins

### Connection Refused

- Ensure Unreal Editor is running
- Check Output Log for server startup messages
- Verify firewall isn't blocking localhost

### Tools Not Appearing

- Call `tools/list` to verify registration
- Check for errors in Output Log
- Restart the editor

### Client not connecting

- Some IDEs like Cursor may need to be started after your Unreal Engine editor as the connection attempt only occurs on startup.

---

## Technical Details

| Specification | Value |
|--------------|-------|
| Engine Version | UE 5.6+ |
| Platforms | Windows, Mac, Linux |
| Module Type | Editor |
| Transport | HTTP/SSE (native) |
| Protocol | JSON-RPC 2.0 / MCP |
| Default Port | 8767 |

### Dependencies

- `PythonScriptPlugin` (included with UE5)
- `EditorScriptingUtilities` (included with UE5)

---

## Contributing

Contributions are welcome! Please read the architecture documentation before submitting PRs.

---

## License

MIT License - See LICENSE file for details.

---

*Give your AI assistant the keys to Unreal Engine.*
