package io.velox.udf;

public class VeloxUdfException extends Exception {
  public VeloxUdfException(String message) {
    super(message);
  }

  public VeloxUdfException(String message, Throwable cause) {
    super(message, cause);
  }
}
