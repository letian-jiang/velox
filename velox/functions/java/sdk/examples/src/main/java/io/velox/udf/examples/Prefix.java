package io.velox.udf.examples;

import io.velox.udf.Nullable;
import io.velox.udf.VeloxScalar;

@VeloxScalar
public final class Prefix {
  @Nullable
  public String evaluate(String prefix, @Nullable String value) {
    return value == null ? null : prefix + value;
  }
}
