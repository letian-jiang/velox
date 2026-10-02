package io.velox.udf.examples;

import io.velox.udf.VeloxScalar;

@VeloxScalar
public final class Add {
  public long evaluate(long left, long right) {
    return left + right;
  }
}
