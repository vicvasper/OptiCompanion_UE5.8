# OptiCompanion

https://github.com/user-attachments/assets/6021dbe9-ea9b-468d-aea2-cdf3ba655a5a


A performance copilot for Unreal Engine 5.8 with a fruit fly brain. A small mascot (by default OC, the OptiCompanion monogram) lives on top of your editor. While you take a break it runs short experiments, learns which optimizations actually work **in your project, on your hardware**, and tells you about them at the next pause. It never changes anything without asking.

> Early development (0.4). Windows only. Editor only: nothing of it ends up in a packaged game. *Español más abajo.*

## What it does

- **Experiments while you work.** What spoils a measurement is the viewport camera moving, not you working. As soon as the camera has been still for 2 seconds (you are editing details, a Blueprint, browsing assets...), the fly tests one change with interleaved A/B blocks and per-pass GPU timings. Moving the camera abandons that experiment; compiling or playing pauses it and it resumes where it left off. While you work it only tries changes it expects to be invisible and never touches components of your level; when you have been away for a while (20 s by default) it also tries riskier ones.
- **Deferred visual check.** Timing comes first. Only promising results get the visual check (three frame grabs, a fraction of a second), which waits for the first half-second pause.
- **Learning from what you do.** Change a render setting or a light's radius yourself and the reflex measures before and after: the fly learns from it (natural experiment) and, if it made the frame faster, tells you right away.
- **Findings, not fixed rules.** A change becomes a finding only when the saving is statistically significant (95% bootstrap interval) and the image does not visibly change (FLIP-inspired perceptual difference, with the frame-to-frame noise of TSR and animated skies subtracted).
- **Save reflex.** Save an asset that makes the frame slower (with the camera still) and the fly flies to the viewport and tells you which save did it.
- **The notebook.** Every finding with its state: new, postponed (remind me in two hours, next session, or when the frame goes over 16.6 ms), applied (crossed off, with undo), resolved by you, stale (the project changed, so it is re-measured before you can apply it) and dismissed.
- **Blueprints, measured in Play.** Blueprint logic only runs while the game does, where the camera never stays still. In Play and Simulate the fly records a few seconds of the engine's own CPU trace (the one Unreal Insights reads) every 45 s, analyses it in the background and adds up each Blueprint class's Tick cost; when you stop, it tells you the worst one and the Inventory page lists them all. Then the fly brain smells each expensive class (its Tick cost, the frame, what is in the level), predicts what a longer Tick Interval (0.1 s) would save and tests the one it expects most from, alternating blocks of frames and timing only the actor tick phase; the result is dopamine for the mushroom body, and a real saving becomes a finding whose Apply changes the Blueprint's class defaults (undoable). It never takes over a trace you started yourself. Off switch: Editor Preferences > OptiCompanion > Play.
- **It shows its work.** A short line under the mascot says what it is testing and how each test went ("Fewer cloud samples: no difference"), without stealing focus. The notebook's **Tested** page keeps every experiment, including the ones not worth telling you about. When a dozen tests in a row find nothing, it says the scene looks clean and tests less often until you change something.
- **Not Clippy.** It only talks at natural pauses (after a save, when leaving PIE, when you come back from a break), never steals focus, lowers its own notice budget when you ignore it, and has quiet, do-not-disturb and professional (no mascot) modes.

**Editor only.** Both modules are `Editor` type, so the plugin is never compiled into, loaded by, or shipped with a packaged game: it leaves no trace in a build. It has no content of its own either. What does reach your game is what you apply: console-variable findings are written to `Config/DefaultEngine.ini`, level and Blueprint findings change your assets, and you save them yourself.

**Speed.** An experiment is four interleaved blocks per variant, about 3.5 seconds, and the next one starts a second and a half later, so the fly gets through roughly ten a minute. The frame profile is reused while the camera and the level stay put instead of measuring it again. Parsing and the bootstrap run on worker threads, so nothing stutters. `Opti.Thorough` (or the setting) measures longer, which only helps with the smallest savings.

## What it can try

78 actions, checked against the engine with `Opti.Catalog`:

- **Rendering settings (65).** Shadows (CSM, virtual shadow map resolution and soft-shadow rays, distance field, contact and capsule shadows), Lumen (screen probes, radiosity, mesh SDF tracing, screen traces, hardware vs software), reflections (Lumen, SSR, SSGI), translucency and Niagara, post process (bloom, motion blur, depth of field, lens flares, TSR history), sky, clouds and volumetric fog, geometry (LOD and view distance, Nanite, foliage and grass density, anisotropy, texture mip bias) and CPU (skeletal LOD, occlusion queries). Applying writes the values to `Config/DefaultEngine.ini` `[SystemSettings]` (checked out first if you use source control).
- **Changes to your level (12).** Rules that pick specific components: tiny objects that cast shadows, small local lights with dynamic shadows, oversized light radii, lights without a draw distance, lights that cast volumetric-fog or translucent shadows, real-time sky capture, small props drawn to the horizon, dense meshes that have LODs but render LOD0. During a nap these are changed in memory only and restored afterwards; nothing is marked dirty. Applying a finding changes the components as one editor transaction (Ctrl+Z works) and you save the level as usual.

- **Blueprints, in Play (1).** A longer Tick Interval on an expensive Blueprint class, tested while the game runs (see above). Applying changes the Blueprint's class defaults and the placed copies that still used the default (Ctrl+Z works); you save the Blueprint.

Actions whose settings do not exist in your engine version, or that have nothing to act on in the current level, are skipped automatically.

## Inventory (from OptiLogger)

The notebook's **Inventory** page lists what is in view (or the whole level): meshes, textures, materials, lights, sounds and post-process volumes, with estimated memory and why each may be expensive (dense meshes without LODs, props placed many times without instancing, 4K/8K textures without virtual texturing, uncompressed HDR, heavy or heavy translucent shaders, dynamic light functions, lights with huge radii or volumetric shadows, long sounds kept in memory). Double-click selects the actor or finds the asset. `Opti.Inventory [all]` does the same from the console and exports JSON.

These are observations, not measurements. The analysis, memory model and visible-only filter come from [OptiLogger](https://github.com/vicvasper/Optilogger_UE5.x); OptiCompanion adds the warnings and feeds the totals (visible texture memory, shader cost) to the fly's antennal lobe, which now smells only what the camera sees.

## Mascots

Pick one in **Editor Preferences > Plugins > OptiCompanion > Mascot** or right-click the mascot: OC (default: the OptiCompanion monogram in black and white, whose O and C are its eyes), bee, fruit fly or ladybug. Every folder in `Resources/Mascots` with a `body.svg` is a theme: add `body.svg`, `wings.svg`, `legs_front.svg` and `icon.svg` (64x64, head up) and it shows up in the list. A theme without wings can add a `theme.json` with `{ "flies": false }`: instead of flying across the screen it shrinks away and pops up where it needs to be. The brain is the fruit fly's whatever it wears.

## The fly brain

Each part of the plugin is modelled on a region of the *Drosophila* brain:

| Region | In the plugin |
|---|---|
| Antennal lobe | 51 inputs, one per real uniglomerular projection-neuron type (DA1, VA1v...): GPU time per pass, thread times, what is in the level, scalability. Divisive normalisation as in Olsen et al. 2010, plus adaptation: each projection neuron also signals how far it is above its own running average, so the few inputs that tell situations apart stand out |
| Mushroom body | Inputs expand to 2000 Kenyon cells through sparse wiring; APL inhibition keeps ~5% active. A broad input per action (like the alpha'/beta' cells that respond to almost any odour) learns what the action is worth in general; the sparse cells learn how this situation differs. Output neurons predict saving, visual cost and whether you like the change. Dopamine is the prediction error (Bennett et al. 2021), so it stops testing what it already knows |
| Memory | Session (fast, fades), project (`Saved/`) and long-term in your user folder, shared by all projects and filled by sleep consolidation after each nap. Weakened when the GPU or engine version changes |
| MBON-a'3 | Novelty detector (Dasgupta et al. 2018): unfamiliar situations make it curious |
| Lateral horn | Innate knowledge in the action catalog, which learning can overrule |
| Central complex | Ring attractor that keeps focus on one area (shadows, lighting, effects...) until it is exhausted |
| Giant fiber | The save reflex |

**Connectome.** The Kenyon cells are wired with the real FlyWire connectome: `Resources/Connectome/pn_kc.csv` holds every uniglomerular projection neuron -> Kenyon cell contact (3+ synapses) in the right calyx of FAFB release 783, which gives 2,337 Kenyon cells over the 51 glomeruli. To regenerate it from newer data, download the FAFB tables from [Codex](https://codex.flywire.ai/api/download) and run:

```
python Tools/extract_pn_kc.py --connections connections_princeton.csv.gz --types consolidated_cell_types.csv.gz
```

If the file is missing, the plugin falls back to a statistical wiring that follows the published claw counts (Caron et al. 2013) with a fixed seed. The connectome data comes from FlyWire (CC-BY 4.0): Dorkenwald et al. 2024, "Neuronal wiring diagram of an adult brain", and Schlegel et al. 2024, "Whole-brain annotation and multi-connectome cell typing of Drosophila", *Nature* 634.

**Does the fly earn its place?** The plugin ships a benchmark that gives the same stream of situations to the fly, to context-free Thompson sampling and to random choice:

```
UnrealEditor-Cmd.exe YourProject.uproject -ExecCmds="Automation RunTests OptiCompanion.Brain;Quit" -unattended -nullrhi
```

Current result with the 77-action catalog (share of the best possible saving, second half of 400 naps, 4 synthetic worlds): fly 83%, Thompson sampling 78%, random 14%. The fly's advantage comes from using context; without projection-neuron adaptation it lost to Thompson sampling (71% vs 77%), which is why adaptation is in. `OptiCompanion.Brain.Tuning` (stress filter) sweeps the two choice weights, `opti.Brain.FocusWeight` and `opti.Brain.Curiosity`. You can also switch the policy in the settings and compare them on your own project; every experiment is logged to `Saved/OptiCompanion/Decisions.csv`.

## Install

1. Copy this folder to `YourProject/Plugins/OptiCompanion`.
2. Open the project. For a Blueprint-only project, accept the prompt to build the plugin (Visual Studio with C++ game development is needed until prebuilt binaries are published).

Settings: **Editor Preferences > Plugins > OptiCompanion**. Right-click the fly for the quick ones, double-click it for the notebook, drag it anywhere.

## Console commands

| Command | What it does |
|---|---|
| `Opti.Noise [Blocks=8] [Frames=60]` | A/A test: how noisy this machine is. About 5% of stats flagged is expected by chance |
| `Opti.Probe <CVar> <A> <B> [Blocks=8] [Frames=60]` | Manual A/B test of a console variable |
| `Opti.Cancel` | Stops a manual probe and restores every console variable |
| `Opti.Catalog` | Lists every action, its current values and whether it can run in this level |
| `Opti.Notebook [findings\|tested\|inventory]` | Opens the notebook on a page |
| `Opti.Reset [all\|keep]` | Starts over: undoes every applied finding, empties the notebook and forgets what it learned here (`all` forgets every project, `keep` leaves your applied changes in place) |
| `Opti.Thorough` | Longer experiments: more evidence, a few seconds more each |

Automated run that closes itself: add `-OptiExitAfterProbe` to a `-game -ExecCmds="Opti.Probe ..."` command line.

## Known limits

- Measurements happen in the editor viewport, which carries editor overhead; findings say where they were measured.
- Applying writes to `[SystemSettings]`, so it affects every scalability level. Per-level application is planned.
- Unreal's `FPlatformMisc::IsRunningOnBattery()` returns true on any laptop with a battery, even when plugged in, so the plugin checks the power line itself.
- Asset-level actions (generating LODs, converting to instances, editing Niagara emitters) are not in the catalog yet; scene actions only change component settings.

## License

Open source. The plugin (code, mascots, phrases and tools) is **MIT**: see [LICENSE](LICENSE). Use it, change it and ship it, keeping the copyright notice.

One file is not mine and has its own licence: `Resources/Connectome/pn_kc.csv`, the Kenyon-cell wiring, is derived from the **FlyWire** FAFB release 783 connectome and is **CC-BY 4.0** (https://creativecommons.org/licenses/by/4.0/). Redistribute it with credit to Dorkenwald et al. 2024 and Schlegel et al. 2024 (*Nature* 634); the changes made are listed in [Resources/Connectome/CREDITS.md](Resources/Connectome/CREDITS.md). Delete that file and the plugin falls back to a statistical wiring of its own, with no strings attached.

The inventory page follows the analysis in [OptiLogger](https://github.com/vicvasper/Optilogger_UE5.x), also mine. Unreal Engine is Epic Games' trademark; this plugin is not affiliated with Epic.

---

## Español

Copiloto de rendimiento para Unreal Engine 5.8 con cerebro de mosca. Una mascota pequeña (por defecto OC, el monograma de OptiCompanion) vive encima de tu editor. Mientras descansas hace pequeños experimentos, aprende qué optimizaciones funcionan de verdad **en tu proyecto y en tu equipo**, y te lo cuenta en la siguiente pausa. Nunca cambia nada sin preguntarte.

- **Experimentos mientras trabajas:** en cuanto la cámara lleva medio segundo quieta, mide el frame y prueba un cambio con bloques A/B intercalados y tiempos por pase de GPU. Puedes seguir trabajando: solo mover la cámara descarta la medida, y si seleccionas o editas algo que está probando, lo restaura al momento.
- **Hallazgos:** solo cuando el ahorro es estadísticamente significativo y la imagen no cambia de forma visible.
- **Reflejo al guardar:** si guardas un asset que empeora el frame, la mosca vuela al viewport y te dice cuál fue.
- **Cuaderno:** nuevo, pospuesto (en dos horas, en la próxima sesión o cuando el frame pase de 16,6 ms), aplicado (tachado y con deshacer), resuelto por ti, caducado (se vuelve a medir antes de aplicarlo) y descartado.
- **Blueprints medidos en Play:** la lógica de los Blueprints solo se ejecuta jugando, donde la cámara no para. En Play y Simulate graba cada 45 s unos segundos del trazado de CPU del propio motor (el de Unreal Insights), lo analiza en segundo plano y suma el coste de Tick de cada clase; al parar te dice la peor y el Inventario las lista. Después el cerebro de la mosca huele cada clase cara (su coste de Tick, el frame, lo que hay en el nivel), predice cuánto ahorraría un Tick Interval de 0,1 s y prueba la que más promete, alternando bloques de frames y midiendo solo los Tick de actores, aprende del resultado y, si ahorra de verdad, crea un hallazgo que al aplicarlo cambia los valores por defecto del Blueprint (con deshacer). Nunca se mete en un trazado que hayas empezado tú.
- **Solo editor:** los dos módulos son de tipo `Editor`, así que el plugin no se compila ni se carga ni deja rastro en un juego empaquetado, y no trae contenido propio. A tu juego solo llega lo que apliques: los ajustes en `Config/DefaultEngine.ini` y los cambios en tus assets, que guardas tú.
- **Rápido:** cada experimento son unos 3,5 segundos y el siguiente empieza segundo y medio después, unos diez por minuto. Reutiliza el perfil del frame mientras no muevas la cámara ni cambies el nivel, y analiza en hilos aparte para no dar tirones. Con `Opti.Thorough` mide más largo.
- **Enseña lo que hace:** una línea bajo la mascota dice qué prueba y cómo ha ido cada prueba, sin interrumpir. La página **Probado** del cuaderno guarda todas las pruebas, también las que no merecía la pena contarte. Si una docena seguida no encuentra nada, avisa de que la escena parece limpia y prueba menos a menudo hasta que cambies algo.
- **Cerebro:** lóbulo antenal con 51 glomérulos reales, cuerpo fungiforme con 2.337 células de Kenyon cableadas con el conectoma real de FlyWire e inhibición APL, dopamina como error de predicción, tres memorias, detector de novedad, cuerno lateral con conocimiento innato y complejo central para el foco.

Ajustes en **Preferencias del editor > Plugins > OptiCompanion**. Clic derecho sobre la mosca para los rápidos, doble clic para el cuaderno, y puedes arrastrarla a cualquier sitio. Las frases están en `Resources/Phrases` (inglés y español) y se pueden traducir sin recompilar.
