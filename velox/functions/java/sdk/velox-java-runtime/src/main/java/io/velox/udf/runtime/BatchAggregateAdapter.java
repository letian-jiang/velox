package io.velox.udf.runtime;

public interface BatchAggregateAdapter extends AutoCloseable {
  int[] create(int groupCount) throws Exception;

  void destroy(byte[] inputIpc) throws Exception;

  void update(byte[] inputIpc) throws Exception;

  void updateSingleGroup(int stateHandle, byte[] inputIpc) throws Exception;

  byte[] serialize(byte[] inputIpc) throws Exception;

  void merge(byte[] inputIpc) throws Exception;

  void mergeSingleGroup(int stateHandle, byte[] inputIpc) throws Exception;

  byte[] finish(byte[] inputIpc) throws Exception;

  @Override
  void close() throws Exception;
}
