import java.awt.Color;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ObjectInputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeModel;

/**
 * Converts a Grasp3D .gsf scene into the Makina scene JSON (docs/SCENE_FORMAT.md).
 *
 * <p>A .gsf is a Java-serialised {@code DefaultTreeModel}, so this has to run against Grasp3D's
 * own classes; the compiled tree under {@code Grasp3D/bin} goes on the classpath. Nothing here
 * writes back into Grasp3D — the tool lives outside that repository so the reference
 * implementation stays untouched.
 *
 * <p>Properties are enumerated generically through {@code SceneElementProperties.getKeys()}
 * rather than from a per-primitive table. A hardcoded table would silently drop any property
 * this tool has not heard of; enumerating means an unknown key still reaches the JSON.
 *
 * <p>Conversion is one-way by design. Grasp3D stays authoritative for .gsf, Makina for JSON.
 */
public final class Gsf2Json {

    /** Ids are assigned in pre-order from 1, so re-running on the same file reproduces them. */
    private int nextId = 1;

    private final List<Map<String, Object>> materials = new ArrayList<>();
    private final Map<String, Integer> materialIndex = new LinkedHashMap<>();
    private final List<String> unsupported = new ArrayList<>();

    /** Element types that carry no geometry and are dropped from the CSG evaluation. */
    private static final String UNSUPPORTED_OP = "Unsupported";

    private static final String[] MATERIAL_KEYS = {
        "Diffuse ", "Alpha ", "Ambient ", "Specular ", "Shinness ", "Emission ", "Texture "
    };

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: Gsf2Json <input.gsf> <output.json>");
            System.exit(2);
        }

        Gsf2Json converter = new Gsf2Json();
        String json = converter.convert(args[0]);

        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[1]), StandardCharsets.UTF_8)) {
            w.write(json);
        }

        System.out.println("ok: " + args[0] + " -> " + args[1]
            + "  (nodes=" + (converter.nextId - 1)
            + ", materials=" + converter.materials.size()
            + ", unsupported=" + converter.unsupported.size()
            + ", dead fields dropped=" + converter.deadDropped + ")");

        // Unsupported elements are reported rather than silently dropped: the count is what tells
        // the reader whether the converted scene is complete.
        for (String u : converter.unsupported) {
            System.out.println("  warning: unsupported element kept as \"" + UNSUPPORTED_OP + "\": " + u);
        }
    }

    private String convert(String path) throws Exception {
        DefaultTreeModel model;
        try (ObjectInputStream in = new ObjectInputStream(new FileInputStream(path))) {
            Object o = in.readObject();
            if (!(o instanceof DefaultTreeModel)) {
                throw new IllegalArgumentException(
                    "expected a DefaultTreeModel at the head of '" + path + "' but found "
                    + (o == null ? "null" : o.getClass().getName()));
            }
            model = (DefaultTreeModel) o;
        }

        DefaultMutableTreeNode root = (DefaultMutableTreeNode) model.getRoot();

        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        sb.append("  \"format\": \"makina-scene\",\n");
        sb.append("  \"version\": 1,\n");
        sb.append("  \"units\": \"meter\",\n");
        sb.append("  \"coordinates\": \"right-handed-y-up\",\n");
        sb.append("  \"angles\": \"degrees\",\n");
        sb.append("  \"source\": { \"tool\": \"gsf2json\", \"file\": ")
          .append(quote(Paths.get(path).getFileName().toString())).append(" },\n");

        String rootJson = node(root, 1);

        sb.append("  \"nextId\": ").append(nextId).append(",\n");
        sb.append("  \"root\": ").append(rootJson).append(",\n");
        sb.append("  \"materials\": ").append(materialsJson(1)).append("\n");
        sb.append("}\n");
        return sb.toString();
    }

    // ---------------------------------------------------------------- tree

    /**
     * @param level indent level at which this object's closing brace sits; its members are one
     *              level deeper. Getting this right matters because the format is meant to be
     *              read in a pull request.
     */
    private String node(DefaultMutableTreeNode n, int level) {
        Object uo = n.getUserObject();
        String indent = "  ".repeat(level);
        String inner = "  ".repeat(level + 1);

        int id = nextId++;
        String op = opOf(uo);
        String name = nameOf(uo);

        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        sb.append(inner).append("\"id\": ").append(id).append(",\n");
        sb.append(inner).append("\"op\": ").append(quote(op)).append(",\n");
        sb.append(inner).append("\"name\": ").append(quote(name));

        if (UNSUPPORTED_OP.equals(op)) {
            unsupported.add(uo == null ? "null" : uo.getClass().getSimpleName() + " \"" + name + "\"");
            sb.append(",\n").append(inner).append("\"originalOp\": ")
              .append(quote(uo == null ? "null" : uo.getClass().getSimpleName()));
        }

        Integer mat = materialOf(uo);
        if (mat != null) {
            sb.append(",\n").append(inner).append("\"material\": ").append(mat);
        }

        for (Map.Entry<String, Object> e : geometryProps(uo).entrySet()) {
            sb.append(",\n").append(inner).append(quote(e.getKey())).append(": ")
              .append(value(e.getValue()));
        }

        if (n.getChildCount() > 0) {
            sb.append(",\n").append(inner).append("\"children\": [\n");
            for (int i = 0; i < n.getChildCount(); i++) {
                sb.append(inner).append("  ")
                  .append(node((DefaultMutableTreeNode) n.getChildAt(i), level + 2));
                if (i < n.getChildCount() - 1) {
                    sb.append(",");
                }
                sb.append("\n");
            }
            sb.append(inner).append("]");
        }

        sb.append("\n").append(indent).append("}");
        return sb.toString();
    }

    // ---------------------------------------------------------------- properties

    /**
     * Properties that Grasp3D writes but never reads back. Carrying them into a new format would
     * preserve a question ("is this used?") that has already been answered, so they are dropped
     * and counted. Each entry was confirmed by reading every consumer, not by guessing.
     *
     * <p>Cone declares two endpoints and two radii, but Cone.render() reads only Radius1 and Z2 —
     * every other read is commented out — and SceneSdf matches that. Disc and Triangle declare a
     * Thickness that nothing reads; PatchSolid derives the thickness it needs from the primitive's
     * size. See PORT_STATUS.md section 3.1.
     */
    private static final Map<String, java.util.Set<String>> DEAD_KEYS = Map.of(
        "Cone", java.util.Set.of("X1 ", "Y1 ", "Z1 ", "X2 ", "Y2 ", "Radius 2 ", "Open "),
        "Disc", java.util.Set.of("Thickness "),
        "Triangle", java.util.Set.of("Thickness ")
    );

    /** Renames that make the surviving names say what they mean. */
    private static final Map<String, Map<String, String>> KEY_RENAMES = Map.of(
        "Cone", Map.of("Radius 1 ", "radius", "Z2 ", "height")
    );

    private int deadDropped = 0;

    /** Everything except the material block; material properties are pooled separately. */
    private Map<String, Object> geometryProps(Object uo) {
        Map<String, Object> out = new LinkedHashMap<>();
        SceneElementProperties props = propsOf(uo);
        if (props == null) {
            return out;
        }

        final String cls = uo.getClass().getSimpleName();
        final java.util.Set<String> dead = DEAD_KEYS.getOrDefault(cls, java.util.Set.of());
        final Map<String, String> renames = KEY_RENAMES.getOrDefault(cls, Map.of());

        for (String key : props.getKeys()) {
            if (isMaterialKey(key)) {
                continue;
            }
            if (dead.contains(key)) {
                deadDropped++;
                continue;
            }
            String name = renames.get(key);
            out.put(name != null ? name : camel(key), props.getValue(key));
        }
        return out;
    }

    /**
     * Pools the material block and returns its index, or null when this element has none.
     * Identical materials collapse to one entry, which keeps the JSON readable on scenes where
     * every primitive was left at the default color.
     */
    private Integer materialOf(Object uo) {
        SceneElementProperties props = propsOf(uo);
        if (props == null) {
            return null;
        }

        Map<String, Object> mat = new LinkedHashMap<>();
        boolean anyValue = false;
        for (String key : MATERIAL_KEYS) {
            Object v = safeValue(props, key);
            if (v != null) {
                anyValue = true;
            }
            if (v != null || "Texture ".equals(key)) {
                mat.put(camel(key), v);
            }
        }
        // A lone null texture is not a material. SceneRoot carries that key and would otherwise
        // pool an empty entry that means nothing.
        if (!anyValue) {
            return null;
        }

        String signature = mat.toString();
        Integer existing = materialIndex.get(signature);
        if (existing != null) {
            return existing;
        }

        int index = materials.size();
        materials.add(mat);
        materialIndex.put(signature, index);
        return index;
    }

    private static Object safeValue(SceneElementProperties props, String key) {
        try {
            return props.getValue(key);
        } catch (RuntimeException e) {
            // A missing key is normal: not every element carries the whole material block.
            return null;
        }
    }

    private static boolean isMaterialKey(String key) {
        for (String m : MATERIAL_KEYS) {
            if (m.equals(key)) {
                return true;
            }
        }
        return false;
    }

    private static SceneElementProperties propsOf(Object uo) {
        return (uo instanceof SceneElement) ? ((SceneElement) uo).getProperties() : null;
    }

    private static String nameOf(Object uo) {
        if (uo instanceof SceneElement) {
            String n = ((SceneElement) uo).getName();
            return n == null ? "" : n;
        }
        return uo == null ? "" : String.valueOf(uo);
    }

    /**
     * Maps a Grasp3D element class to a scene-format op. Anything without a closed-form SDF, or
     * without geometry at all, becomes "Unsupported" and is counted (SCENE_FORMAT.md §3.4).
     */
    private static String opOf(Object uo) {
        if (uo == null) {
            return "SceneRoot";
        }
        String cls = uo.getClass().getSimpleName();
        switch (cls) {
            case "Box": case "Sphere": case "Cylinder": case "Cone": case "Torus":
            case "Plane": case "Disc": case "Triangle":
            case "Translate": case "Rotate": case "Scale":
            case "Merge": case "Difference": case "Intersection":
            case "Label": case "SceneRoot":
                return cls;
            default:
                // SuperEllipsoidBox, Light, Camera, and anything added later.
                return UNSUPPORTED_OP;
        }
    }

    // ---------------------------------------------------------------- json

    private String materialsJson(int depth) {
        if (materials.isEmpty()) {
            return "[]";
        }
        String indent = "  ".repeat(depth + 1);
        StringBuilder sb = new StringBuilder("[\n");
        for (int i = 0; i < materials.size(); i++) {
            sb.append(indent).append("{ \"id\": ").append(i);
            for (Map.Entry<String, Object> e : materials.get(i).entrySet()) {
                sb.append(", ").append(quote(e.getKey())).append(": ").append(value(e.getValue()));
            }
            sb.append(" }");
            if (i < materials.size() - 1) {
                sb.append(",");
            }
            sb.append("\n");
        }
        sb.append("  ".repeat(depth)).append("]");
        return sb.toString();
    }

    private static String value(Object v) {
        if (v == null) {
            return "null";
        }
        if (v instanceof Color) {
            Color c = (Color) v;
            return "[" + c.getRed() + ", " + c.getGreen() + ", " + c.getBlue() + "]";
        }
        if (v instanceof Double || v instanceof Float) {
            double d = ((Number) v).doubleValue();
            if (d == Math.rint(d) && !Double.isInfinite(d)) {
                // 1.0 rather than 1 keeps the value unambiguously floating point in the JSON.
                return String.format("%.1f", d);
            }
            return trimZeros(String.format("%.9f", d));
        }
        if (v instanceof Number || v instanceof Boolean) {
            return String.valueOf(v);
        }
        return quote(String.valueOf(v));
    }

    private static String trimZeros(String s) {
        if (s.indexOf('.') < 0) {
            return s;
        }
        s = s.replaceAll("0+$", "");
        return s.endsWith(".") ? s + "0" : s;
    }

    /**
     * Grasp3D property names carry a trailing space and are otherwise free text.
     *
     * <p>"Major radius " -> "majorRadius", "LIGHT_0" -> "light_0". An all-caps token is
     * lowercased whole rather than by its first letter, or LIGHT_0 would come out as lIGHT_0.
     *
     * <p>"Shinness " is a spelling mistake in Grasp3D's own key. It is corrected here rather
     * than carried into a new format, where it would be permanent (SCENE_FORMAT.md §3.5).
     */
    private static String camel(String key) {
        String trimmed = key.trim();
        String corrected = SPELLING_FIXES.getOrDefault(trimmed, trimmed);

        String[] parts = corrected.split("\\s+");
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < parts.length; i++) {
            String p = parts[i];
            if (p.isEmpty()) {
                continue;
            }
            boolean allCaps = p.equals(p.toUpperCase()) && p.length() > 1;
            String body = allCaps ? p.toLowerCase() : p;
            if (i == 0) {
                sb.append(Character.toLowerCase(body.charAt(0))).append(body.substring(1));
            } else {
                sb.append(Character.toUpperCase(body.charAt(0))).append(body.substring(1));
            }
        }
        return sb.toString();
    }

    private static final Map<String, String> SPELLING_FIXES = Map.of("Shinness", "Shininess");

    private static String quote(String s) {
        StringBuilder sb = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                default:
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
            }
        }
        return sb.append('"').toString();
    }
}
