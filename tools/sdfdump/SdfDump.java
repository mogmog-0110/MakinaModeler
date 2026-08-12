import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ObjectInputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;

import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeModel;

/**
 * Samples Grasp3D's SceneSdf over a lattice and writes the results, so the C++ port can be
 * checked against the implementation it was ported from (Phase 1's exit criterion).
 *
 * <p>The sample coordinates travel in the file rather than being regenerated on the other side.
 * Two languages computing "the same" lattice independently would drift in the last bits, and then
 * every reported mismatch would have to be argued about instead of trusted.
 *
 * <p>Output is one sample per line: {@code x y z distance}, with %.17g so a double survives the
 * round trip exactly. An empty subtree is written as {@code inf}.
 */
public final class SdfDump {

    private static final int COARSE_STEPS = 11;
    private static final int FINE_STEPS   = 9;

    /// Fallback half-extent when the scene has no bounded geometry (a lone Plane, say).
    private static final double FALLBACK_EXTENT = 3.0;

    /// How far past the scene's own bounds the coarse lattice reaches, so samples land outside
    /// the solid as well as inside it.
    private static final double MARGIN = 0.5;

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: SdfDump <input.gsf> <output.sdf.txt>");
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

        // The lattice follows the scene rather than a fixed box. A fixed box either misses a
        // spread-out scene entirely -- every sample far outside, every comparison passing for the
        // wrong reason -- or wastes most of its samples on empty space.
        double[] b = SceneBounds.worldBounds(root);
        double[] lo = new double[3];
        double[] hi = new double[3];
        if (b == null) {
            for (int i = 0; i < 3; i++) {
                lo[i] = -FALLBACK_EXTENT;
                hi[i] = FALLBACK_EXTENT;
            }
        } else {
            for (int i = 0; i < 3; i++) {
                double span = Math.max(b[i + 3] - b[i], 1e-3);
                lo[i] = b[i] - span * MARGIN;
                hi[i] = b[i + 3] + span * MARGIN;
            }
        }

        int written = 0;
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[1]), StandardCharsets.UTF_8)) {
            w.write("# makina sdf reference dump\n");
            w.write("# source: " + args[0] + "\n");
            w.write(String.format("# bounds: [%.4f %.4f %.4f] .. [%.4f %.4f %.4f]%n",
                lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]));
            w.write("# columns: x y z distance   (distance 'inf' means the subtree is empty)\n");

            written += lattice(w, root, lo, hi, COARSE_STEPS);

            // A second, tighter lattice over the middle half, where the surfaces mostly are.
            double[] flo = new double[3];
            double[] fhi = new double[3];
            for (int i = 0; i < 3; i++) {
                double mid = (lo[i] + hi[i]) * 0.5;
                double half = (hi[i] - lo[i]) * 0.25;
                flo[i] = mid - half;
                fhi[i] = mid + half;
            }
            written += lattice(w, root, flo, fhi, FINE_STEPS);
            written += surfaceBand(w, root, lo, hi);
        }

        System.out.println("ok: " + args[0] + " -> " + args[1] + "  (samples=" + written + ")");
    }

    /**
     * Samples in a thin band around the surface, which a lattice alone never reaches.
     *
     * <p>A lattice spends nearly all of its points far from any surface, where two implementations
     * agree easily because both are returning a large positive number. The interesting
     * disagreements are where the sign flips: a boolean seam, a coincident face, the axis of a
     * torus. Each seed point is walked onto the surface along the gradient, and the landing point
     * is emitted together with a pair just inside and just outside it, covering both signs.
     *
     * <p>Seeds far from any geometry are skipped rather than marched: from far away the walk drifts
     * and lands somewhere arbitrary, which is a sample nobody chose.
     */
    private static int surfaceBand(Writer w, DefaultMutableTreeNode root, double[] lo, double[] hi)
            throws Exception {
        double diag = Math.sqrt(sq(hi[0] - lo[0]) + sq(hi[1] - lo[1]) + sq(hi[2] - lo[2]));
        double seedRange = diag * 0.25;   // how far from a surface a seed may start
        double offset = Math.max(1e-4, diag * 1e-3);

        int steps = 13;
        int count = 0;
        for (int i = 0; i < steps; i++) {
            for (int j = 0; j < steps; j++) {
                for (int k = 0; k < steps; k++) {
                    double[] p = {
                        lo[0] + (hi[0] - lo[0]) * i / (steps - 1),
                        lo[1] + (hi[1] - lo[1]) * j / (steps - 1),
                        lo[2] + (hi[2] - lo[2]) * k / (steps - 1),
                    };

                    double d0 = SceneSdf.eval(root, p);
                    if (Double.isInfinite(d0) || Math.abs(d0) > seedRange) {
                        continue;
                    }

                    double[] s = project(root, p);
                    if (s == null) {
                        continue;
                    }
                    double[] g = gradient(root, s, offset);
                    if (g == null) {
                        continue;
                    }

                    emit(w, root, s);
                    emit(w, root, new double[] { s[0] - g[0] * offset,
                                                 s[1] - g[1] * offset,
                                                 s[2] - g[2] * offset });
                    emit(w, root, new double[] { s[0] + g[0] * offset,
                                                 s[1] + g[1] * offset,
                                                 s[2] + g[2] * offset });
                    count += 3;
                }
            }
        }
        return count;
    }

    /** Walks p onto the zero level set. Returns null if it does not settle. */
    private static double[] project(DefaultMutableTreeNode root, double[] p) {
        double[] q = { p[0], p[1], p[2] };
        for (int iter = 0; iter < 12; iter++) {
            double d = SceneSdf.eval(root, q);
            if (Double.isInfinite(d)) {
                return null;
            }
            if (Math.abs(d) < 1e-9) {
                return q;
            }
            double[] g = gradient(root, q, 1e-5);
            if (g == null) {
                return null;
            }
            q[0] -= g[0] * d;
            q[1] -= g[1] * d;
            q[2] -= g[2] * d;
        }
        return Math.abs(SceneSdf.eval(root, q)) < 1e-6 ? q : null;
    }

    /** Normalised central-difference gradient, or null where the field is flat or empty. */
    private static double[] gradient(DefaultMutableTreeNode root, double[] p, double h) {
        double[] g = new double[3];
        for (int a = 0; a < 3; a++) {
            double[] lo = { p[0], p[1], p[2] };
            double[] hi = { p[0], p[1], p[2] };
            lo[a] -= h;
            hi[a] += h;
            double dl = SceneSdf.eval(root, lo);
            double dh = SceneSdf.eval(root, hi);
            if (Double.isInfinite(dl) || Double.isInfinite(dh)) {
                return null;
            }
            g[a] = (dh - dl) / (2 * h);
        }
        double len = Math.sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (len < 1e-12) {
            return null;
        }
        g[0] /= len;
        g[1] /= len;
        g[2] /= len;
        return g;
    }

    private static void emit(Writer w, DefaultMutableTreeNode root, double[] p) throws Exception {
        double d = SceneSdf.eval(root, p);
        w.write(String.format("%.17g %.17g %.17g %s%n", p[0], p[1], p[2],
            Double.isInfinite(d) ? "inf" : String.format("%.17g", d)));
    }

    private static double sq(double v) {
        return v * v;
    }

    private static int lattice(Writer w, DefaultMutableTreeNode root,
                               double[] lo, double[] hi, int steps) throws Exception {
        int count = 0;
        for (int i = 0; i < steps; i++) {
            for (int j = 0; j < steps; j++) {
                for (int k = 0; k < steps; k++) {
                    double x = lo[0] + (hi[0] - lo[0]) * i / (steps - 1);
                    double y = lo[1] + (hi[1] - lo[1]) * j / (steps - 1);
                    double z = lo[2] + (hi[2] - lo[2]) * k / (steps - 1);

                    double d = SceneSdf.eval(root, new double[] { x, y, z });

                    w.write(String.format("%.17g %.17g %.17g %s%n", x, y, z,
                        Double.isInfinite(d) ? "inf" : String.format("%.17g", d)));
                    count++;
                }
            }
        }
        return count;
    }
}
