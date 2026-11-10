// ---- NEE: sampling delle luci e pdf per la MIS --------------------------------

// Distanza a cui si mette il Sole per il raggio d'ombra: e' all'infinito, ma basta oltre
// qualunque geometria. Combacia con il tMax di TraceRay.
const float SUN_DISTANCE = 100000.0;

// La mappa emissive nel punto q di una sfera o di un quad emissivo, 1 se il materiale non ne ha.
// Emission.rgb della luce e' colore * strength, uguale su tutta la superficie: la mappa si
// applica qui, nel punto campionato, con le stesse UV che vedrebbe un raggio che lo colpisce,
// altrimenti NEE e raggio BSDF stimerebbero due luci diverse e la MIS sarebbe biased (issue #15).
vec3 EmitterTexture(Light L, vec3 q) {
    int index = L.Meta.z;
    if(L.Meta.x == 0) {
        Sphere sphere = Spheres[index];
        Material m = Materials[clamp(int(sphere.Mat), 0, numMaterials - 1)];
        if(m.EmissiveTexture < 0.0)
            return vec3(1.0);
        vec2 uv = SphereUV(normalize(q - sphere.Position.xyz));
        return textureLod(AlbedoTextures, vec3(uv, m.EmissiveTexture), SECONDARY_TEXTURE_LOD).rgb;
    }
    Quad quad = Quads[index];
    Material m = Materials[clamp(int(quad.MaterialIndex), 0, numMaterials - 1)];
    if(m.EmissiveTexture < 0.0)
        return vec3(1.0);
    vec2 uv = QuadUV(quad, q - quad.PositionLLC.xyz);
    return textureLod(AlbedoTextures, vec3(uv, m.EmissiveTexture), SECONDARY_TEXTURE_LOD).rgb;
}

// ---- i triangoli emissivi delle mesh (ADR 0025) ----

// Il materiale di un triangolo: il suo, o quello dell'istanza, clampato come in HandleHit
int TriangleMaterial(int triangle, int mesh) {
    float own = TriUVs[triangle].A.z;
    return clamp(own >= 0.0 ? int(own) : int(Meshes[mesh].MaterialIndex), 0, numMaterials - 1);
}

// Il peso con cui la alias table della sua istanza sceglie il triangolo: area locale per luminanza
// dell'emissione. Deve combaciare con BuildLights in RenderScene.cpp, soglia compresa: la pdf
// della NEE e quella della MIS sull'hit lo ricalcolano da qui, e devono essere la stessa.
float EmissiveWeight(int triangle, int mesh) {
    TrianglePosition t = TriPositions[triangle];
    float area = 0.5 * length(cross(t.B.xyz - t.A.xyz, t.C.xyz - t.A.xyz));
    Material m = Materials[TriangleMaterial(triangle, mesh)];
    float lum = dot(m.EmissiveColor.rgb * m.EmissiveStrenght, vec3(0.2126, 0.7152, 0.0722));
    return lum > 1e-4 ? area * lum : 0.0;
}

// I vertici del triangolo in world space e cross(b - a, c - a): la sua lunghezza e' il doppio
// dell'area world, la direzione la normale geometrica
vec3 WorldTriangle(int triangle, int mesh, out vec3 a, out vec3 b, out vec3 c) {
    TrianglePosition t = TriPositions[triangle];
    mat4 M = Meshes[mesh].Transform;
    a = (M * vec4(t.A.xyz, 1.0)).xyz;
    b = (M * vec4(t.B.xyz, 1.0)).xyz;
    c = (M * vec4(t.C.xyz, 1.0)).xyz;
    return cross(b - a, c - a);
}

// pdf in solid angle, da p, di aver campionato il punto q del triangolo: la voce fra numLights,
// il triangolo col suo peso fra quelli dell'istanza, il punto uniforme sull'area world
float EmissiveTrianglePdf(int triangle, int mesh, float total, vec3 crossed, float dist2, float cosLgt) {
    float areaWorld = 0.5 * length(crossed);
    if(cosLgt < 1e-6 || areaWorld < 1e-12 || total <= 0.0)
        return 0.0;
    return (1.0 / float(numLights)) * (EmissiveWeight(triangle, mesh) / total) * dist2 / (cosLgt * areaWorld);
}

// Il punto da raggiungere col raggio d'ombra: q staccato di 1 mm dall'emettitore lungo la sua
// normale, dalla parte di p. Mirare a q lungo wi da un'origine gia' spostata farebbe correre il
// raggio parallelo a p -> q, e su un emettitore visto di taglio colpirebbe l'emettitore stesso
// prima del margine: luce persa, tanto piu' quanto la superficie e' curva (issue #16).
vec3 ShadowTarget(vec3 q, vec3 n, vec3 p) {
    return q + (dot(n, p - q) >= 0.0 ? n : -n) * 0.001;
}

// Campiona un emettitore (scelto uniformemente) e restituisce direzione, distanza, radianza
// e pdf in solid angle (gia' comprensiva di 1/numLights). false = campione non valido.
// target = dove mira il raggio d'ombra (ShadowTarget; per il Sole un punto lontano lungo wi).
// delta = luce punto o spot: nessun raggio BSDF la puo' colpire, quindi peso MIS 1. Per loro
// Lrad e' gia' l'irradianza sul punto (I / d^2) e pdf la sola probabilita' di sceglierla.
bool SampleLight(vec3 p, out vec3 wi, out float dist, out vec3 Lrad, out float pdf, out bool delta, out vec3 target) {
    wi = vec3(0.0, 1.0, 0.0); dist = 0.0; Lrad = vec3(0.0); pdf = 0.0; delta = false; target = p;
    if(numLights == 0)
        return false;

    int li = min(int(GetRandomFloat() * float(numLights)), numLights - 1);
    Light L = Lights[li];
    Lrad = L.Emission.rgb;
    float pickPdf = 1.0 / float(numLights);

    if(L.Meta.x == 5) {
        // ---- istanza di mesh emissiva: un triangolo dalla alias table, un punto uniforme su di lui ----
        int mesh = L.Meta.z;
        int count = int(L.U.x);
        float u = GetRandomFloat() * float(count);
        int k = min(int(u), count - 1);
        EmissiveSample entry = EmissiveSamples[L.Meta.w + k];
        int tri = (u - float(k)) < entry.Prob ? entry.Triangle : entry.AliasTriangle;

        vec3 a, b, c;
        vec3 crossed = WorldTriangle(tri, mesh, a, b, c);
        float su = sqrt(GetRandomFloat()), sv = GetRandomFloat();
        float b0 = 1.0 - su, b1 = sv * su, b2 = 1.0 - b0 - b1; // pesi di A, B, C come in HandleHit
        vec3 q = a * b0 + b * b1 + c * b2;
        vec3 dvec = q - p;
        float dist2 = dot(dvec, dvec);
        if(dist2 < 1e-10)
            return false;
        dist = sqrt(dist2);
        wi = dvec / dist;
        float cosLgt = abs(dot(normalize(crossed), wi)); // due lati, come l'emissione sull'hit
        pdf = EmissiveTrianglePdf(tri, mesh, L.Emission.a, crossed, dist2, cosLgt);
        if(pdf <= 0.0)
            return false;
        TriangleUV tuv = TriUVs[tri];
        Lrad = SampleEmission(Materials[TriangleMaterial(tri, mesh)], tuv.A.xy * b0 + tuv.B.xy * b1 + tuv.C.xy * b2, SECONDARY_TEXTURE_LOD);
        target = ShadowTarget(q, normalize(crossed), p);
        return true;
    }

    if(L.Meta.x >= 3) {
        // ---- luce punto o spot (ADR 0009): una sola direzione possibile ----
        vec3 w = L.P.xyz - p;
        float d2 = dot(w, w);
        if(d2 < 1e-8)
            return false;
        dist = sqrt(d2);
        wi = w / dist;
        float falloff = 1.0;
        if(L.Meta.x == 4) {
            // sfumatura del cono come nella specifica glTF: lineare in cos, poi al quadrato
            float cd = dot(L.U.xyz, -wi);
            float t = clamp((cd - L.U.w) / max(L.V.x - L.U.w, 1e-3), 0.0, 1.0);
            falloff = t * t;
            if(falloff <= 0.0)
                return false;
        }
        Lrad *= falloff / d2;
        pdf = pickPdf;
        delta = true;
        target = L.P.xyz;
        return true;
    }

    if(L.Meta.x == 2) {
        // ---- Sole: cono uniforme attorno alla sua direzione, uguale da ogni punto ----
        float cosMax = L.P.w;
        float cosT = 1.0 - GetRandomFloat() * (1.0 - cosMax);
        float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
        float phi = 2.0 * PI * GetRandomFloat();

        vec3 tx, ty; BuildBasis(L.P.xyz, tx, ty);
        wi = normalize(tx * (sinT * cos(phi)) + ty * (sinT * sin(phi)) + L.P.xyz * cosT);
        dist = SUN_DISTANCE;
        pdf = pickPdf / max(L.Emission.a, 1e-12);
        target = p + wi * SUN_DISTANCE;
        return true;
    }

    if(L.Meta.x == 0) {
        // ---- sfera: cone sampling (Shirley) ----
        vec3 c = L.P.xyz; float r = L.P.w;
        vec3 w = c - p;
        float d2 = dot(w, w);
        float d = sqrt(d2);
        if(d <= r)               // punto dentro la sfera-luce: caso degenere, salta
            return false;

        float cosMax = sqrt(max(0.0, 1.0 - (r * r) / d2));
        float u1 = GetRandomFloat(), u2 = GetRandomFloat();
        float cosT = 1.0 - u1 * (1.0 - cosMax);
        float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
        float phi = 2.0 * PI * u2;

        vec3 wz = w / d;
        vec3 tx, ty; BuildBasis(wz, tx, ty);
        wi = normalize(tx * (sinT * cos(phi)) + ty * (sinT * sin(phi)) + wz * cosT);

        // distanza all'intersezione con la sfera lungo wi (radice vicina)
        float b = dot(wi, p - c);
        float disc = b * b - (d2 - r * r);
        dist = (disc >= 0.0) ? (-b - sqrt(disc)) : (d - r);
        if(dist <= 0.0)
            dist = d - r;

        vec3 q = p + wi * dist;
        Lrad *= EmitterTexture(L, q);
        float solid = 2.0 * PI * (1.0 - cosMax);
        pdf = pickPdf / max(solid, 1e-8);
        target = ShadowTarget(q, normalize(q - c), p);
        return true;
    } else {
        // ---- quad: area sampling ----
        float u1 = GetRandomFloat(), u2 = GetRandomFloat();
        vec3 q = L.P.xyz + u1 * L.U.xyz + u2 * L.V.xyz;
        vec3 dvec = q - p;
        float dist2 = dot(dvec, dvec);
        dist = sqrt(dist2);
        wi = dvec / dist;
        float cosLgt = abs(dot(L.Normal.xyz, wi)); // emettitore two-sided (come l'emissione sull'hit)
        if(cosLgt < 1e-6)
            return false;
        Lrad *= EmitterTexture(L, q);
        pdf = pickPdf * dist2 / (cosLgt * L.Emission.a);
        target = ShadowTarget(q, L.Normal.xyz, p);
        return true;
    }
}

// pdf (solid angle, incl. 1/numLights) con cui il light sampling avrebbe generato la direzione
// da fromPos verso l'emettitore effettivamente colpito da un raggio BSDF. Serve al termine MIS
// sull'emissione. 0 se l'oggetto colpito non e' un emettitore campionabile.
float LightPdfToward(vec3 fromPos, HitInfo hit) {
    if(numLights == 0)
        return 0.0;
    // un triangolo di mesh: la sua istanza sa qual e' la sua voce, niente ricerca (ADR 0025)
    if(hit.ObjectType == 2) {
        int li = int(Meshes[hit.ObjectIndex].Emitter.x);
        if(li < 0 || hit.Triangle < 0)
            return 0.0;
        vec3 a, b, c;
        vec3 crossed = WorldTriangle(hit.Triangle, hit.ObjectIndex, a, b, c);
        vec3 dvec = hit.HitPosition - fromPos;
        float dist2 = dot(dvec, dvec);
        float cosLgt = abs(dot(normalize(crossed), normalize(dvec)));
        return EmissiveTrianglePdf(hit.Triangle, hit.ObjectIndex, Lights[li].Emission.a, crossed, dist2, cosLgt);
    }
    for(int i = 0; i < numLights; i++) {
        if(Lights[i].Meta.y != hit.ObjectType || Lights[i].Meta.z != hit.ObjectIndex)
            continue;
        float pickPdf = 1.0 / float(numLights);
        if(Lights[i].Meta.x == 0) {
            vec3 c = Lights[i].P.xyz; float r = Lights[i].P.w;
            float d2 = dot(c - fromPos, c - fromPos);
            if(d2 <= r * r)
                return 0.0;
            float cosMax = sqrt(max(0.0, 1.0 - (r * r) / d2));
            float solid = 2.0 * PI * (1.0 - cosMax);
            return pickPdf / max(solid, 1e-8);
        } else {
            vec3 dvec = hit.HitPosition - fromPos;
            float dist2 = dot(dvec, dvec);
            float cosLgt = abs(dot(Lights[i].Normal.xyz, normalize(dvec)));
            if(cosLgt < 1e-6)
                return 0.0;
            return pickPdf * dist2 / (cosLgt * Lights[i].Emission.a);
        }
    }
    return 0.0;
}
