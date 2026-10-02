package io.velox.udf.runtime;

import java.io.PrintWriter;
import java.io.StringWriter;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Paths;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

public final class Bootstrap {
  private static final ConcurrentMap<String, ClassLoader> LOADERS =
      new ConcurrentHashMap<String, ClassLoader>();

  private Bootstrap() {}

  public static BatchScalarAdapter loadScalar(String jar, String implementation)
      throws Exception {
    return BatchScalarAdapter.class.cast(load(jar, implementation));
  }

  public static BatchAggregateAdapter loadAggregate(String jar, String implementation)
      throws Exception {
    return BatchAggregateAdapter.class.cast(load(jar, implementation));
  }

  private static Object load(String jar, String implementation) throws Exception {
    ClassLoader loader = LOADERS.get(jar);
    if (loader == null) {
      URL url = Paths.get(jar).toUri().toURL();
      ClassLoader candidate =
          new URLClassLoader(new URL[] {url}, Bootstrap.class.getClassLoader());
      ClassLoader prior = LOADERS.putIfAbsent(jar, candidate);
      loader = prior == null ? candidate : prior;
      if (prior != null) {
        ((URLClassLoader) candidate).close();
      }
    }
    Class<?> adapter = Class.forName(implementation + "__VeloxAdapter", true, loader);
    return adapter.getDeclaredConstructor().newInstance();
  }

  public static String formatThrowable(Throwable throwable) {
    StringWriter buffer = new StringWriter();
    PrintWriter writer = new PrintWriter(buffer);
    throwable.printStackTrace(writer);
    writer.flush();
    return buffer.toString();
  }
}
