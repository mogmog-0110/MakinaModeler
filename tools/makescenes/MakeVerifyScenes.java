import java.io.BufferedOutputStream;
import java.io.FileOutputStream;
import java.io.ObjectOutputStream;

import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeModel;

/**
 * Builds .gsf scenes that cover the code paths Grasp3D's own sample files never touch.
 *
 * <p>Written as a program rather than clicked together in the GUI so the corpus is reproducible
 * and reviewable: a binary blob in version control cannot be diffed, and nobody can tell later
 * which case it was meant to cover.
 *
 * <p>A .gsf is a serialised {@code DefaultTreeModel}, exactly what GRASP_MAIN.save writes, so
 * these files load like any hand-made scene.
 *
 * <p><b>What this cannot check.</b> Two implementations agreeing proves nothing if the scene is
 * geometrically degenerate — a triangle collapsed to a line, a rotation of effectively zero. The
 * shapes have to be looked at once by a human. Angles are deliberately 37 and 40 degrees rather
 * than 0/45/90 so a mistake cannot hide behind a symmetry.
 */
public final class MakeVerifyScenes {

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: MakeVerifyScenes <output directory>");
            System.exit(2);
        }
        String dir = args[0];

        write(dir + "/verify_faces.gsf", faces());
        write(dir + "/verify_plane.gsf", plane());
        write(dir + "/verify_transforms.gsf", transforms());
    }

    // ---------------------------------------------------------------- scenes

    /**
     * Zero-thickness faces, standalone only. Disc and Triangle are display-only in Makina: they
     * have no interior, so they are not valid CSG operands (PORT_STATUS.md). This scene exists to
     * check their distance functions, not their behaviour under a boolean.
     */
    private static DefaultMutableTreeNode faces() {
        DefaultMutableTreeNode root = newRoot();

        // Solid disc, then an annulus: the hole takes a different branch of the distance function.
        root.add(at(-3, 0, 0, new Disc(1.0, 0.0)));
        root.add(at(0, 0, 0, new Disc(1.0, 0.4)));

        // Tilted out of every axis plane so an edge-on view cannot hide a degenerate result.
        root.add(at(3, 0, 0, new Triangle(0.0, 1.2, 0.0,
                                          -1.0, -0.6, 0.5,
                                          1.0, -0.6, -0.5)));
        return root;
    }

    /**
     * Plane is a half space and therefore a genuine solid — inside is y &lt;= Y — so unlike Disc
     * and Triangle it is a valid CSG operand. Both booleans are exercised.
     */
    private static DefaultMutableTreeNode plane() {
        DefaultMutableTreeNode root = newRoot();

        // Box spans y 0..2; subtracting the half space below y=1 should leave its top half.
        DefaultMutableTreeNode diff = node(new Difference());
        diff.add(node(new Box(2.0, 2.0, 2.0)));
        diff.add(node(new Plane(1.0)));
        root.add(at(-3, 0, 0, diff));

        // Sphere intersected with the half space below y=0: the lower hemisphere.
        DefaultMutableTreeNode inter = node(new Intersection());
        inter.add(node(new Sphere(1.2)));
        inter.add(node(new Plane(0.0)));
        root.add(at(2, 0, 0, inter));

        // Ground, well below the rest, to show a standalone infinite plane.
        root.add(node(new Plane(-1.5)));
        return root;
    }

    /**
     * Transform paths the sample corpus barely touches: single-axis rotation about each axis, a
     * non-uniform Scale, a mirroring Scale, a negative cone height, and a nested stack.
     */
    private static DefaultMutableTreeNode transforms() {
        DefaultMutableTreeNode root = newRoot();

        root.add(at(-7, 0, 0, wrap(new Rotate("X", 37.0), new Box(1.0, 1.0, 1.0))));
        root.add(at(-4, 0, 0, wrap(new Rotate("Y", 37.0), new Box(1.0, 1.0, 1.0))));
        root.add(at(-1, 0, 0, wrap(new Rotate("Z", 37.0), new Box(1.0, 1.0, 1.0))));

        // Non-uniform Scale: the distance correction falls back to the smallest axis factor, which
        // understates the distance. Nothing in the sample corpus exercises it (PLAN.md R-13).
        root.add(at(2, 0, 0, wrap(new Scale(2.0, 0.5, 1.5), new Sphere(0.8))));

        // Negative factor on one axis: a mirror. scaleFactorOf takes absolute values, so this
        // checks that the sign does not leak into the distance.
        root.add(at(5, 0, 0, wrap(new Scale(-1.0, 1.0, 1.0), new Cone(0.8, 1.5))));

        // Negative height: the cone is evaluated flipped, a branch of its own.
        root.add(at(8, 0, 0, new Cone(0.8, -1.5)));

        // Nested transforms, so the composition order is checked and not just each one alone.
        DefaultMutableTreeNode nested =
            wrap(new Rotate("Y", 40.0), wrap(new Scale(1.5, 1.0, 1.0),
                                             new Cylinder(1.0, -1.0, 0.5)));
        root.add(at(11, 0, 0, nested));

        return root;
    }

    // ---------------------------------------------------------------- helpers

    private static DefaultMutableTreeNode newRoot() {
        return new DefaultMutableTreeNode(new SceneRoot("Scene"));
    }

    private static DefaultMutableTreeNode node(Object element) {
        return new DefaultMutableTreeNode(element);
    }

    /** Wraps a child under a transform node. */
    private static DefaultMutableTreeNode wrap(Object transform, Object child) {
        DefaultMutableTreeNode t = node(transform);
        t.add(node(child));
        return t;
    }

    private static DefaultMutableTreeNode wrap(Object transform, DefaultMutableTreeNode child) {
        DefaultMutableTreeNode t = node(transform);
        t.add(child);
        return t;
    }

    /** Places an element at a world offset, so the cases sit side by side and can be told apart. */
    private static DefaultMutableTreeNode at(double x, double y, double z, Object element) {
        return wrap(new Translate(x, y, z), element);
    }

    private static DefaultMutableTreeNode at(double x, double y, double z,
                                             DefaultMutableTreeNode subtree) {
        return wrap(new Translate(x, y, z), subtree);
    }

    private static void write(String path, DefaultMutableTreeNode root) throws Exception {
        DefaultTreeModel model = new DefaultTreeModel(root);
        try (FileOutputStream fos = new FileOutputStream(path);
             ObjectOutputStream oos = new ObjectOutputStream(new BufferedOutputStream(fos))) {
            oos.writeObject(model);
            oos.flush();
        }
        System.out.println("wrote " + path + "  (" + count(root) + " nodes)");
    }

    private static int count(DefaultMutableTreeNode n) {
        int total = 1;
        for (int i = 0; i < n.getChildCount(); i++) {
            total += count((DefaultMutableTreeNode) n.getChildAt(i));
        }
        return total;
    }
}
