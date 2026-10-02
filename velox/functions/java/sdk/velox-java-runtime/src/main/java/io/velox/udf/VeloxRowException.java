package io.velox.udf;

public final class VeloxRowException extends Exception {
  public VeloxRowException(String message) {
    super(message);
  }

  public VeloxRowException(String message, Throwable cause) {
    super(message, cause);
  }
}
