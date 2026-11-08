# PathTracing Renderer

Path tracer GPU su compute shader (Vulkan, con ray query). Accumula progressivamente campioni in un'immagine HDR e la mostra dopo tonemapping. Questo glossario fissa il linguaggio dei sottosistemi; non è una specifica.

## Language

### Rendering

**Accumulation**:
L'integratore temporale per pixel mantenuto dal renderer: conserva **irradianza** demodulata. A camera ferma è una media progressiva che converge alla verità, illimitatamente; in movimento non si azzera più ma riusa il frame precedente tramite riproiezione.
_Avoid_: averaging, temporal blend

**Sample count**:
Il numero di campioni integrati nel pixel finora; guida il peso della storia (`1/N`) e quanto il denoiser deve intervenire. Illimitato da fermo, limitato dal cap sulla storia riproiettata.
_Avoid_: frame count, iterations

**Temporal reprojection**:
Il recupero del valore di un pixel dal frame precedente, trovandolo tramite la posizione mondo ricostruita dal Depth e la view-projection di quel frame. È ciò che sostituisce l'azzeramento dell'accumulazione al movimento.
_Avoid_: motion vectors, TAA, warping

**History**:
Il contenuto dell'accumulazione al frame precedente, con i suoi Depth e Normal, tenuto per essere riproiettato e validato nel frame corrente.
_Avoid_: previous frame, cache, feedback buffer

**Disocclusion**:
Il caso in cui la storia riproiettata non descrive più la stessa superficie (depth o normale non combaciano, o il pixel cade fuori schermo): la storia va scartata e il pixel riparte da zero.
_Avoid_: cache miss, invalidation

**Ghosting**:
La scia lasciata da storia stantia che la validazione non ha intercettato: il pixel mostra ancora luce appartenente a ciò che copriva prima.
_Avoid_: smearing, trailing, lag

### Camera

**Inquadratura**:
La camera della scena: posizione, direzione, campo visivo e obiettivo. Si salva con la scena ed è quella del render finale.
_Avoid_: scene camera (ok nel codice), shot camera, render camera

**Camera della vista**:
Dove si trova la viewport mentre si lavora. Agganciata, naviga spostando l'Inquadratura; sganciata, gira libera senza toccarla. Usa sempre l'obiettivo dell'Inquadratura. Non si salva.
_Avoid_: editor camera, navigation camera, viewport camera (ok)

### Scena

**Scena**:
La descrizione di cosa c'è da rendere: gli oggetti, le luci, il Sole, l'inquadratura e l'ambiente. Cita gli asset senza contenerli; insieme alla libreria degli asset è quello che un file di scena salva.
_Avoid_: world, level, document

**Libreria degli asset**:
Dove stanno gli asset di una scena aperta: geometria, materiali, texture. Sta accanto alla scena, non dentro.
_Avoid_: asset manager, resource cache

**Oggetto**:
Una cosa della scena che si seleziona e si salva: sfera, quad, box, mesh, luce puntiforme, oppure un materiale. Ha un id, che è la sua identità e non cambia mai, e un nome unico fra gli oggetti del suo tipo.
_Avoid_: entity, node, item, primitive (per l'oggetto intero)

**Documento**:
La scena aperta vista come file: da dove viene, e se ha modifiche non salvate. Navigare la vista non è una modifica.
_Avoid_: project, workspace

**Scena integrata**:
Una scena che arriva col programma (Cornell box, Sponza...). È un file di scena come gli altri, in una cartella che la UI elenca, con un numero e un nome.
_Avoid_: preset (è il nome dei preset di render), demo scene

**Asset**:
Qualcosa che gli oggetti usano senza contenerlo: la geometria di una mesh, un materiale, una texture. Ha un id; più oggetti possono citare lo stesso asset.
_Avoid_: resource, data-block (ok come analogia con Blender)

**Istanza** (di mesh):
L'oggetto della scena che mostra una geometria in un punto: dove sta e quale asset di geometria usa. Due istanze dello stesso asset dividono i triangoli.
_Avoid_: copy, clone

**Material slot**:
Un posto per un materiale nella geometria di una mesh: i triangoli citano lo slot, lo slot cita il materiale. I triangoli senza slot usano il materiale dell'istanza.

**Render scene**:
La scena come la consuma la GPU: triangoli, light list, strutture di accelerazione e texture, derivati dalla scena e dagli asset. Non si modifica né si salva: si ricostruisce quando la scena cambia.
_Avoid_: GPU scene, scene proxy

### Superfici

**Shading normal**:
La normale con cui si valutano BSDF, NEE e l'AOV Normal: quella interpolata, perturbata dal normal map se il materiale ne ha uno.
_Avoid_: bump normal, perturbed normal

**Geometric normal**:
La normale della superficie senza normal map. Decide da che parte della superficie partono i raggi e quali direzioni restano sopra la superficie vera. Coincide con la Shading normal quando non c'è normal map.
_Avoid_: face normal, true normal

**Normal map**:
Una texture che descrive in spazio tangente lo scostamento della Shading normal dalla Geometric normal. Cambia come la superficie riflette la luce, non la sua forma: silhouette e ombre portate restano quelle della geometria.
_Avoid_: bump map, height map

**Alpha mask**:
Una soglia sull'alpha di una texture che decide dove la superficie esiste: sotto la soglia il raggio la attraversa come se non ci fosse, ombre comprese. È tutto o niente: non c'è trasparenza parziale.
_Avoid_: transparency, opacity, cutout (ok)

### Impostazioni

**Impostazioni di render**:
Come si calcola l'immagine: integratore, accumulazione, denoiser, film. La stessa scena con impostazioni diverse dà la stessa immagine a qualità diversa. Si salvano con la scena.
_Avoid_: quality settings, render options

**Preset**:
Un insieme di Impostazioni di render dato come punto di partenza (Draft, Preview, Final). Non è una modalità: dopo averlo scelto se ne cambia qualunque campo.
_Avoid_: profile, mode

**Vista**:
Cosa si sta guardando nella viewport, per esempio una vista di debug di un AOV. Non cambia mai l'immagine calcolata e non si salva.
_Avoid_: display mode, view settings (ok nel codice)

### Illuminazione diretta

**Next Event Estimation (NEE)**:
A ogni vertice non-speculare del cammino si stima la luce diretta campionando **esplicitamente** un emettitore, invece di aspettare che un rimbalzo casuale lo colpisca. È ciò che rende utilizzabili le scene con luci piccole: senza, l'unico modo per illuminare una superficie è che un raggio diffuso becchi la luce per caso — evento raro, quindi rumore altissimo. Inattiva se la scena non ha geometria emissiva.
_Avoid_: direct lighting pass, shadow pass

**Emitter**:
Qualunque sorgente di luce che la NEE sa campionare esplicitamente: una primitiva emissiva (sfera o quad), il **Sole** oppure una **Luce puntiforme**. Una primitiva emissiva non è un tipo di luce a parte: è la stessa geometria che appare e riflette, di cui si sa anche campionare la superficie. L'ambiente uniforme **non** è un emitter (campionarlo non ridurrebbe la varianza).
_Avoid_: light source, lamp

**Sole**:
L'Emitter senza geometria: una luce direzionale all'infinito, con un disco angolare piccolo ma non nullo che si vede nel cielo. Illumina con la stessa direzione ogni punto della scena.
_Avoid_: directional light (ok), sun light, infinite light

**Luce puntiforme**:
Un Emitter delta senza geometria: un punto che irradia in tutte le direzioni (luce punto) o dentro un cono (spot). Nessun raggio la colpisce, quindi non si vede e illumina solo tramite la NEE; le ombre che proietta sono nette.
_Avoid_: lamp, bulb, point source

**Light sampling**:
Il campionamento di una direzione verso un emitter con la sua pdf: cono (solid angle) per le sfere, area per i quad. Lo shadow ray verifica poi che la luce sia visibile.
_Avoid_: light ray, sampling the lamp

**Multiple Importance Sampling (MIS)**:
La combinazione pesata di due stime della stessa luce diretta — una per **light sampling**, una per **BSDF sampling** — con la balance heuristic `p_a / (p_a + p_b)`. Toglie il doppio conteggio e prende il meglio di entrambe: la superficie diffusa illuminata dall'area light la risolve il light sampling, il riflesso speculare della luce lo risolve il BSDF sampling. Senza MIS l'una o l'altra tecnica esploderebbe di rumore o conterebbe due volte.
_Avoid_: weighting, blending strategies

**Shadow ray**:
Un raggio di sola occlusione (any-hit concettuale) tra la superficie e il punto campionato sull'emitter: se qualcosa lo interrompe prima della luce, il contributo diretto è zero. Non trasporta colore.
_Avoid_: occlusion ray (ok), light ray

### Denoising

**Denoiser**:
Un **filtro di sola visualizzazione** applicato all'immagine accumulata: produce l'immagine mostrata a schermo senza mai riscrivere nell'accumulazione. Non fa parte dell'integratore — la riproiezione temporale, che è feedback, appartiene all'Accumulation, non al denoiser.
_Avoid_: temporal filter, feedback filter

**À-trous filter**:
Il filtro wavelet edge-avoiding (Dammertz 2010) che realizza il denoiser: poche passate spaziali a passo crescente, fermate ai bordi dalle guide.
_Avoid_: blur, bilateral, gaussian

**AOV** (feature buffer):
Canali ausiliari per-pixel emessi dal compute accanto al colore per guidare il denoiser: qui Albedo, Normal, Depth.
_Avoid_: G-buffer, render target, channel

**Albedo** (AOV):
Il colore base della prima superficie colpita, senza illuminazione; guida per la demodulazione e per l'edge-stop di colore.
_Avoid_: confondere con la texture albedo del materiale, che ne è solo un input; base color

**Demodulation**:
Separare la luce dal dettaglio di superficie: si divide il colore per l'albedo ottenendo **irradianza** (liscia), e la si rimoltiplica per l'albedo solo al display, così le texture restano nitide. Avviene a monte, nell'accumulazione: è l'irradianza a essere integrata nel tempo e filtrata. Demodulazione e rimodulazione devono usare lo **stesso albedo clampato**, o il giro non è invertibile e i materiali scuri perdono energia.
_Avoid_: albedo division

**Irradiance**:
Il colore diviso per l'albedo: la sola illuminazione, senza il dettaglio di superficie. È ciò che l'accumulazione conserva, la riproiezione trasporta e l'à-trous filtra.
_Avoid_: radiance, lighting, indirect

### Interoperabilità

**glTF (interscambio)**:
Il formato con cui geometria e materiali entrano nel renderer da strumenti esterni (Blender in primis). NON leggiamo il `.blend`: è Blender a esportare glTF. L'import mappa il PBR metallic-roughness sul nostro Material e i nodi/primitive sulle MeshInfo. Vedi [[ADR 0003]].
_Avoid_: modello, asset, formato nativo

**Live-link**:
Il sync fra Blender e il renderer: un addon esporta glTF a ogni salvataggio, il renderer osserva il file e ricarica. Read-only verso Blender: il renderer non riscrive mai la scena.
_Avoid_: bridge, plugin, hot-reload

**Edge-stopping**:
Le funzioni di peso (su normal, depth, albedo) che fermano la sfocatura dell'à-trous alle discontinuità delle feature, preservando i bordi.
_Avoid_: edge detection, feature weighting
