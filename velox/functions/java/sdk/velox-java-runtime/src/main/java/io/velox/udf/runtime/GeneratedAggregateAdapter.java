package io.velox.udf.runtime;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public abstract class GeneratedAggregateAdapter<S>
    implements BatchAggregateAdapter {
  private long nextHandle = 1;
  private final Map<Integer, S> states = new HashMap<Integer, S>();

  protected abstract S createState() throws Exception;

  protected abstract void destroyState(S state) throws Exception;

  protected abstract void updateState(
      S state,
      ArrowSupport.InputBatch batch,
      int row,
      int argumentOffset) throws Exception;

  protected abstract byte[] serializeState(S state) throws Exception;

  protected abstract void mergeState(S state, byte[] intermediate)
      throws Exception;

  protected abstract Object finishState(S state) throws Exception;

  protected abstract TypeSpec finishType();

  @Override
  public final int[] create(int groupCount) throws Exception {
    if (groupCount < 0) {
      throw new IllegalArgumentException("negative Java UDAF group count");
    }
    int[] handles = new int[groupCount];
    List<Integer> created = new ArrayList<Integer>(groupCount);
    try {
      for (int i = 0; i < groupCount; ++i) {
        if (nextHandle > 0xffffffffL) {
          throw new IllegalStateException("Java UDAF state handle space is exhausted");
        }
        S state = createState();
        int handle = (int) nextHandle++;
        states.put(handle, state);
        created.add(handle);
        handles[i] = handle;
      }
      return handles;
    } catch (Exception failure) {
      for (Integer handle : created) {
        S state = states.remove(handle);
        if (state != null) {
          try {
            destroyState(state);
          } catch (Exception ignored) {
            // Preserve the original create failure.
          }
        }
      }
      throw failure;
    }
  }

  @Override
  public final void destroy(byte[] inputIpc) throws Exception {
    try (ArrowSupport.InputBatch batch = ArrowSupport.decode(inputIpc)) {
      for (int row = 0; row < batch.getRowCount(); ++row) {
        int handle = ArrowSupport.readHandle(batch, 0, row);
        S state = states.remove(handle);
        if (state != null) {
          destroyState(state);
        }
      }
    }
  }

  @Override
  public final void update(byte[] inputIpc) throws Exception {
    try (ArrowSupport.InputBatch batch = ArrowSupport.decode(inputIpc)) {
      for (int row = 0; row < batch.getRowCount(); ++row) {
        int handle = ArrowSupport.readHandle(batch, 0, row);
        updateState(requireState(handle), batch, row, 1);
      }
    }
  }

  @Override
  public final void updateSingleGroup(int stateHandle, byte[] inputIpc)
      throws Exception {
    S state = requireState(stateHandle);
    try (ArrowSupport.InputBatch batch = ArrowSupport.decode(inputIpc)) {
      for (int row = 0; row < batch.getRowCount(); ++row) {
        updateState(state, batch, row, 0);
      }
    }
  }

  @Override
  public final byte[] serialize(byte[] inputIpc) throws Exception {
    try (ArrowSupport.InputBatch batch = ArrowSupport.decode(inputIpc)) {
      byte[][] output = new byte[batch.getRowCount()][];
      for (int row = 0; row < batch.getRowCount(); ++row) {
        output[row] = serializeState(
            requireState(ArrowSupport.readHandle(batch, 0, row)));
        if (output[row] == null) {
          throw new IllegalStateException("Java UDAF serialize returned null");
        }
      }
      return ArrowSupport.encodeBinary(output);
    }
  }

  @Override
  public final void merge(byte[] inputIpc) throws Exception {
    try (ArrowSupport.InputBatch batch = ArrowSupport.decode(inputIpc)) {
      for (int row = 0; row < batch.getRowCount(); ++row) {
        byte[] intermediate = ArrowSupport.readBinary(batch, 1, row);
        if (intermediate != null) {
          mergeState(
              requireState(ArrowSupport.readHandle(batch, 0, row)),
              intermediate);
        }
      }
    }
  }

  @Override
  public final void mergeSingleGroup(int stateHandle, byte[] inputIpc)
      throws Exception {
    S state = requireState(stateHandle);
    try (ArrowSupport.InputBatch batch = ArrowSupport.decode(inputIpc)) {
      for (int row = 0; row < batch.getRowCount(); ++row) {
        byte[] intermediate = ArrowSupport.readBinary(batch, 0, row);
        if (intermediate != null) {
          mergeState(state, intermediate);
        }
      }
    }
  }

  @Override
  public final byte[] finish(byte[] inputIpc) throws Exception {
    try (ArrowSupport.InputBatch batch = ArrowSupport.decode(inputIpc)) {
      Object[] output = new Object[batch.getRowCount()];
      for (int row = 0; row < batch.getRowCount(); ++row) {
        output[row] = finishState(
            requireState(ArrowSupport.readHandle(batch, 0, row)));
      }
      return ArrowSupport.encodeValues(finishType(), output, null);
    }
  }

  private S requireState(int handle) {
    if (handle == 0) {
      throw new IllegalArgumentException("invalid Java UDAF state handle 0");
    }
    S state = states.get(handle);
    if (state == null) {
      throw new IllegalArgumentException(
          "unknown Java UDAF state handle " + Integer.toUnsignedString(handle));
    }
    return state;
  }

  @Override
  public void close() throws Exception {
    Exception failure = null;
    for (S state : states.values()) {
      try {
        destroyState(state);
      } catch (Exception error) {
        if (failure == null) {
          failure = error;
        }
      }
    }
    states.clear();
    if (failure != null) {
      throw failure;
    }
  }
}
