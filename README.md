# SpecialAgent

**Connect AI to Unreal Engine 5**

Full Python API access • ~294 tools across 45 services • Visual feedback loop

---

## What's new

- **Phase 4 — stability & screenshots:**
  - **Reliable screenshots.** `screenshot/capture` and `screenshot/save` now always target the Level Editor viewport (never a focused material/Niagara/preview window) and synchronously repaint it before reading pixels, so captures no longer return stale or black frames — you no longer need a separate `viewport/force_redraw`. Image responses also report the correct MIME type (JPEG bytes are no longer mislabelled as PNG), fixing blank/garbled images in strict vision clients.
  - **No more silent hangs.** Every game-thread call is now time-bounded: a stalled/backgrounded editor returns a clean error instead of wedging the request forever. The editor's "Use Less CPU when in Background" throttle is disabled on startup so requests keep flowing while the editor is not the foreground window.
  - **Crash hardening.** Fixed an SSE use-after-free on client disconnect, a re-entrant PIE single-step crash, unchecked viewport-client casts (focusing a non-level viewport no longer crashes), unguarded JSON accessors on the network path, and several reachable `check()` aborts from hostile/invalid tool arguments. `reflection/call_function` now only invokes safe, directly-callable functions.
  - **Truthful results.** Tool failures now surface through the MCP error channel (`isError`) instead of being reported as success. Malformed coordinates are rejected instead of silently becoming `(0,0,0)`.
  - **Docs overhaul (~294 tools).** Every tool description was rewritten against researched UE5.7 best practices to a consistent shape (what it does + return shape → `Params:` with units/defaults → `Workflow:` chaining → `Warning:` side-effects). Fixed dozens of schema-vs-handler mismatches (wrong/undocumented params), wrong units and defaults, inaccurate return shapes, and removed all references to deprecated APIs (EditorLevelLibrary/EditorAssetLibrary/etc.) in favor of the modern Editor Subsystems. Actor results now include a clear `actor_label` and `path` handle.
  - **Durable, undoable edits.** Mutating tools (spawn/transform/property/material/component/datatable/lighting/sky/post-process/decal/foliage/landscape/PCG/gameplay/sequencer, etc.) now wrap their changes in an undo transaction and mark the owning package dirty, so edits show up for save and can be reverted with Ctrl+Z instead of silently evaporating on reload.
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

Edit `Config/DefaultSpecialAgent.ini` (shipped with the plugin) to customize:

```ini
[/Script/SpecialAgent.SpecialAgentSettings]
; Auto-start the MCP server when the editor launches
ServerEnabled=true

; Server port (change if 8767 is in use)
ServerPort=8767
```

To override these per project without editing the plugin, add the same
`[/Script/SpecialAgent.SpecialAgentSettings]` section with `ServerEnabled` /
`ServerPort` to your project's `Config/DefaultGame.ini` — project settings take
precedence over the plugin defaults.

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

Committed references (browsable on GitHub):

| Document | Description |
|----------|-------------|
| [docs/TOOLS.md](docs/TOOLS.md) | Reference of all ~319 MCP tools across 45 services |
| [Content/Docs/ue5_python_best_practices.md](Content/Docs/ue5_python_best_practices.md) | `python/execute` golden rules, modern subsystem map, units, gotchas |
| [docs/ue5_python_api_reference.md](docs/ue5_python_api_reference.md) | Build-accurate `unreal` API (methods + signatures) for key classes — regenerate with `Content/Python/generate_api_reference.py` |
| [Content/Docs/deprecations.md](Content/Docs/deprecations.md) | Deprecated → modern API mapping |
| [Content/Docs/idioms/](Content/Docs/idioms/) | Cookbook entries (spawn_actor, material_params, transactions, …) |

In-editor, the server also exposes these as MCP resources (call `resources/list`): `mcp://unreal/cheatsheet`, `mcp://unreal/best_practices`, `mcp://unreal/deprecations`, `mcp://unreal/services` (live tool index), `mcp://unreal/idioms/*`.

The authoritative tool list is always available at runtime via `tools/list`; for the exact live API of any class use the `python/inspect_class` / `search_symbol` / `get_function_signature` tools.

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

### Screenshots look black, stale, or fail to decode

- `screenshot/capture` and `screenshot/save` now force a viewport repaint and report the correct image MIME type, so this should be fixed. If a capture is still black, confirm a **Level Editor viewport** is open (the tools target it specifically) and that the level has finished loading.
- Pass `force_redraw: false` only if you intentionally want the last drawn frame (faster, but may be stale).

### Requests hang or time out

- The plugin disables the editor's *"Use Less CPU when in Background"* setting on startup so the game thread keeps ticking while the editor is not the foreground window — the most common cause of MCP requests appearing to hang. If you re-enable it manually, background requests will be slow again.
- Game-thread operations are time-bounded (~120s); a genuinely stuck editor (open modal dialog, long blocking import/compile) returns a structured error rather than hanging forever. Avoid tools that open modal dialogs (`utility/show_dialog`, interactive save-as) while driving the editor unattended.

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
