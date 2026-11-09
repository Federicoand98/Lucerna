# Blender → Lucerna

Sync a senso unico: **Blender esporta, Lucerna legge**. Vedi [ADR 0003](../../docs/adr/0003-interop-blender-via-gltf.md) e [ADR 0024](../../docs/adr/0024-blender-avviato-da-lucerna.md).

## Uso

1. In Lucerna: **File > Open in Blender…**. Lucerna lancia Blender con [`lucerna_link.py`](lucerna_link.py) caricato solo per quella sessione: non c'è niente da installare.
2. In Blender apri qualsiasi `.blend`. All'apertura, a ogni salvataggio (`Ctrl+S`) e con **Sync now** (sidebar `N` > tab **Lucerna**, o l'indicatore nella status bar di Lucerna) la scena si esporta in `<cartella del .blend>/lucerna_live/scene.gltf` e Lucerna la apre.
3. La status bar di Lucerna mostra lo stato: in attesa di un `.blend`, collegato (con l'ultimo sync), Blender chiuso. Cliccandola: Sync now, Show export folder, Disconnect.

Il primo sync di un `.blend` prende la camera attiva di Blender; i successivi tengono l'inquadratura di Lucerna. Le modifiche fatte in Lucerna a una scena di Blender si perdono al sync successivo: il viewport lo segnala quando ce ne sono.

## Il path di Blender

**Edit > Preferences > Blender**. Vuoto, Lucerna lo cerca: nel `PATH`, in `/usr/bin`, `/snap/bin`, nei `bin` di flatpak, in `/opt/blender*` e `~/Applications/blender*`; su Windows in `Program Files/Blender Foundation/Blender *`.

## Come si parlano

Lucerna passa all'addon il path di un manifest (`--lucerna-manifest`, un JSON nella cartella temporanea, uno per processo di Lucerna). Dopo ogni export completo l'addon lo riscrive con una rename atomica, così Lucerna non legge mai un glTF a metà. Sync now in Lucerna crea `<manifest>.request`, che l'addon controlla ogni mezzo secondo. Il formato è descritto in testa a `lucerna_link.py`.

## Cosa passa

Mesh (triangoli, normali, UV), transform, materiali Principled BSDF nel sottoinsieme che il BSDF di Lucerna rappresenta (base color e texture, metallic, roughness, IOR, transmission, emissione, normal map, alpha), luci punto, spot e sole (`KHR_lights_punctual`, ADR 0009), la camera attiva. Node graph procedurali, subsurface, coat, sheen e anisotropy non passano.

L'export è `GLTF_SEPARATE` (`.gltf` + `.bin` + texture): Lucerna carica le texture da file.
