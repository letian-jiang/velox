package io.velox.udf.examples;

import io.velox.udf.VeloxRowException;
import io.velox.udf.VeloxScalar;

@VeloxScalar
public final class CheckedDivide {
  public long evaluate(long left, long right) throws VeloxRowException {
    if (right == 0) {
      throw new VeloxRowException("division by zero");
    }
    return left / right;
  }
}
