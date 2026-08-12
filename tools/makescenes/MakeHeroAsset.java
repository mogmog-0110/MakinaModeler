import java.io.BufferedOutputStream;
import java.io.FileOutputStream;
import java.io.ObjectOutputStream;

import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeModel;

/**
 * Builds the hero asset: a machined flange, the model the weathering demo is shot on.
 *
 * <p>The shape is chosen for what the geometry fields need to show rather than for its own sake:
 *
 * <ul>
 *   <li><b>sharp convex edges</b> — the hub rim, the bore lip, the mouth of every bolt hole —
 *       where curvature is high and worn metal goes bright</li>
 *   <li><b>a rounded outer edge</b>, cut with a torus, so the demo shows wear falling off with
 *       radius instead of a single hard line</li>
 *   <li><b>deep concave cavities</b> — the annular groove and the bores — where ambient occlusion
 *       collapses and grime collects</li>
 *   <li><b>an overhanging lip</b> around the top, so the outer wall is sheltered from above.
 *       Occlusion alone calls that wall open, because sideways it is; only a directional sky
 *       term sees the difference, and that is what makes grime run down it</li>
 *   <li><b>broad flat up-facing surfaces</b> for dust to settle on</li>
 *   <li><b>a thin web</b> between the groove and the underside, so thickness-driven scatter has
 *       somewhere to show</li>
 * </ul>
 *
 * <p>The bolt holes are one Merge with six children rather than six nested Differences. That is
 * how a person would group them, and it is also what the flattener's balanced folding is for.
 */
public final class MakeHeroAsset {

    private static final int BOLT_COUNT = 6;
    private static final double PLATE_RADIUS = 2.0;
    private static final double PLATE_HALF = 0.25;
    private static final double HUB_RADIUS = 0.9;
    private static final double HUB_TOP = 0.45;
    private static final double BORE_RADIUS = 0.55;
    private static final double BOLT_RING = 1.45;
    private static final double BOLT_RADIUS = 0.18;
    /// The cap ring overhangs the plate wall by this much, which is the only thing on the part
    /// that shelters a vertical surface from above. Without it the sky term reads 1 everywhere on
    /// the rim and the streaks the shader derives have nowhere to appear.
    private static final double LIP_OVERHANG = 0.16;
    private static final double LIP_THICKNESS = 0.09;

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: MakeHeroAsset <output directory> [variants]");
            System.exit(2);
        }

        write(args[0] + "/hero_flange.gsf", 1.15);

        // A sweep of the groove radius. Nothing about the material changes between these files;
        // only one number in the model does. Whatever the weathering does across the sequence, it
        // does because the shape moved -- which is the claim the whole approach rests on, and the
        // one a baked texture cannot make.
        int variants = args.length > 1 ? Integer.parseInt(args[1]) : 0;
        for (int i = 0; i < variants; i++) {
            double radius = 0.80 + 0.62 * i / Math.max(1, variants - 1);
            write(String.format("%s/hero_sweep_%d.gsf", args[0], i), radius);
        }
    }

    private static void write(String path, double grooveRadius) throws Exception {
        DefaultMutableTreeNode root = new DefaultMutableTreeNode(new SceneRoot("Scene"));
        root.add(flange(grooveRadius));

        DefaultTreeModel model = new DefaultTreeModel(root);
        try (FileOutputStream fos = new FileOutputStream(path);
             ObjectOutputStream oos = new ObjectOutputStream(new BufferedOutputStream(fos))) {
            oos.writeObject(model);
            oos.flush();
        }
        System.out.println("wrote " + path + "  (" + count(root) + " nodes)");
    }

    private static DefaultMutableTreeNode flange(double grooveRadius) {
        DefaultMutableTreeNode diff = node(new Difference());

        // Body: the plate with its hub standing on top.
        DefaultMutableTreeNode body = node(new Merge());
        body.add(node(new Cylinder(PLATE_HALF, -PLATE_HALF, PLATE_RADIUS)));
        body.add(node(new Cylinder(HUB_TOP, PLATE_HALF - 0.01, HUB_RADIUS)));
        // Cap ring, wider than the plate: it puts a roof over the outer wall.
        body.add(node(new Cylinder(PLATE_HALF, PLATE_HALF - LIP_THICKNESS,
                                   PLATE_RADIUS + LIP_OVERHANG)));
        diff.add(body);

        // Central bore, run well past both faces so the ends are open.
        diff.add(node(new Cylinder(HUB_TOP + 0.5, -PLATE_HALF - 0.5, BORE_RADIUS)));

        // Bolt holes as one group. Six operands is what makes balanced folding worth doing.
        DefaultMutableTreeNode bolts = node(new Merge());
        for (int i = 0; i < BOLT_COUNT; i++) {
            double a = 2.0 * Math.PI * i / BOLT_COUNT;
            bolts.add(at(BOLT_RING * Math.cos(a), 0.0, BOLT_RING * Math.sin(a),
                         new Cylinder(PLATE_HALF + 0.5, -PLATE_HALF - 0.5, BOLT_RADIUS)));
        }
        diff.add(bolts);

        // Annular groove on the top face: a torus sunk just below the surface leaves a rounded
        // channel, which is where grime should end up.
        // Sunk below the face rather than grazing it, so the occlusion inside actually collapses;
        // the original 0.08 channel barely dipped and the grime had nothing to fill. Not deeper
        // than this: at 0.22 the channel floor reaches the top of the bolt bores and the holes
        // come out as slots.
        diff.add(at(0.0, PLATE_HALF - 0.02, 0.0, new Torus(grooveRadius, 0.13)));

        // Outer edge break: a torus ringing the rim rounds the top outer corner, so wear can fade
        // across it rather than stopping at a crease.
        diff.add(at(0.0, PLATE_HALF, 0.0, new Torus(PLATE_RADIUS + LIP_OVERHANG, 0.10)));

        // Relief pocket underneath, leaving a thin web above it.
        diff.add(at(0.0, -PLATE_HALF - 0.30, 0.0, new Cylinder(0.34, -0.34, 1.55)));

        return diff;
    }

    private static DefaultMutableTreeNode node(Object element) {
        return new DefaultMutableTreeNode(element);
    }

    private static DefaultMutableTreeNode at(double x, double y, double z, Object element) {
        DefaultMutableTreeNode t = node(new Translate(x, y, z));
        t.add(node(element));
        return t;
    }

    private static int count(DefaultMutableTreeNode n) {
        int total = 1;
        for (int i = 0; i < n.getChildCount(); i++) {
            total += count((DefaultMutableTreeNode) n.getChildAt(i));
        }
        return total;
    }
}
