package io.velox.udf.runtime;

public interface BatchScalarAdapter extends AutoCloseable {
  byte[] apply(byte[] inputIpc) throws Exception;

  @Override
  void close() throws Exception;
}
