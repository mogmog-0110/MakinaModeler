import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ObjectInputStream;
import java.io.OutputStream;

import javax.swing.tree.DefaultTreeModel;

/**
 * Writes the geometry half of Grasp3D's POV-Ray export for a .gsf, as the reference the C++ port
 * checks against.
 *
 * <p>Geometry only: no camera, no lights, no #includes. Those come from the caller in both
 * implementations and carry nothing about the model, so comparing them would only compare two
 * copies of the same constant. What is worth comparing is the object tree -- which primitives, in
 * what order, wrapped in which booleans, carrying which transforms and materials.
 *
 * <p>This is the same PovExporter the application's export menu uses. It became callable without a
 * GUI so that this tool could exist.
 */
public final class PovDump {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: PovDump <input.gsf> <output.pov>");
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

        try (OutputStream out = new FileOutputStream(args[1])) {
            out.write(("// grasp3d geometry reference for " + args[0] + "\n\n").getBytes("UTF-8"));
            PovExporter.traverse(model, model.getRoot(), out, new StringBuffer());
        }

        System.out.println("ok: " + args[0] + " -> " + args[1]);
    }
}
