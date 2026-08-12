import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ObjectInputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.List;

import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeModel;

/**
 * Runs Grasp3D's SceneMeasure over a .gsf and writes the results for the C++ port to check against.
 *
 * <p>Subtrees are addressed by <b>the ordinal of the root's direct child</b>, not by a node index.
 * The two implementations lay their trees out differently -- Makina allocates a node's children
 * contiguously so that a flat array can carry them, which the Java tree does not -- so an index
 * means different things on the two sides. A child ordinal means the same thing in both.
 *
 * <p>None of these measurements uses random numbers except the overlap volume, which seeds
 * java.util.Random with a constant. The C++ side reproduces that generator, so even the volume is
 * compared exactly rather than statistically.
 */
public final class MeasureDump {

    /// Pairs are quadratic and each gap costs thousands of SDF evaluations, so the sweep is capped.
    private static final int MAX_CHILDREN = 6;

    private static final double TOL = 1e-3;
    private static final double GROUND_Y = 0.0;

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: MeasureDump <input.gsf> <output.measure.txt>");
            System.exit(2);
        }

        DefaultTreeModel model;
        try (ObjectInputStream in = new ObjectInputStream(new FileInputStream(args[0]))) {
            Object o = in.readObject();
            if (!(o instanceof DefaultTreeModel)) {
                throw new IllegalArgumentException(
                    "expected a DefaultTreeModel at the head of '" + args[0] + "' but found "
                    + (o == null ? "null" : o.getClass().getName()));
            }
            model = (DefaultTreeModel) o;
        }

        DefaultMutableTreeNode root = (DefaultMutableTreeNode) model.getRoot();
        int childCount = Math.min(root.getChildCount(), MAX_CHILDREN);

        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[1]), StandardCharsets.UTF_8)) {
            w.write("# makina measure reference dump\n");
            w.write("# source: " + args[0] + "\n");
            w.write("# subtrees are addressed by the ordinal of the root's direct child\n");
            w.write(String.format("children %d%n", childCount));

            for (int i = 0; i < childCount; i++) {
                for (int j = i + 1; j < childCount; j++) {
                    DefaultMutableTreeNode a = (DefaultMutableTreeNode) root.getChildAt(i);
                    DefaultMutableTreeNode b = (DefaultMutableTreeNode) root.getChildAt(j);

                    SceneMeasure.GapResult g = SceneMeasure.gap(a, b);
                    w.write(String.format("gap %d %d %s %d%n", i, j, num(g.distance), g.samples));

                    SceneMeasure.OverlapResult o = SceneMeasure.overlap(a, b, TOL);
                    w.write(String.format("overlap %d %d %s %s %d%n", i, j,
                        num(o.maxPenetration), num(o.volume), o.aabbIntersects ? 1 : 0));
                }
            }

            List<SceneMeasure.FloatItem> items = SceneMeasure.floating(root, GROUND_Y, TOL);
            w.write(String.format("floating %s %s %d%n", num(GROUND_Y), num(TOL), items.size()));
            for (int i = 0; i < items.size(); i++) {
                SceneMeasure.FloatItem it = items.get(i);
                w.write(String.format("float %d %s %d %d %s%n", i, num(it.minY),
                    it.supported ? 1 : 0, it.sunk ? 1 : 0, num(it.gapToNearest)));
            }

            for (int axis = 0; axis < 3; axis++) {
                SceneMeasure.SymmetryResult sy = SceneMeasure.symmetry(root, axis, 0.0, TOL);
                w.write(String.format("symmetry %d %s %s %s %d %d%n", axis, num(0.0),
                    num(sy.maxDev), num(sy.meanDev), sy.samples, sy.offenders.size()));
            }
        }

        System.out.println("ok: " + args[0] + " -> " + args[1]
            + "  (children=" + childCount + ")");
    }

    /** %.17g so a double survives the round trip; infinity is written as a word. */
    private static String num(double v) {
        if (Double.isInfinite(v)) {
            return v > 0 ? "inf" : "-inf";
        }
        if (Double.isNaN(v)) {
            return "nan";
        }
        return String.format("%.17g", v);
    }
}
