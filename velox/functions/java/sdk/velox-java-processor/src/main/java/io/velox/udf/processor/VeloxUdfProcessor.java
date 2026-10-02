package io.velox.udf.processor;

import io.velox.udf.VeloxAggregate;
import io.velox.udf.VeloxScalar;
import java.io.IOException;
import java.io.Writer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import javax.annotation.processing.AbstractProcessor;
import javax.annotation.processing.Filer;
import javax.annotation.processing.RoundEnvironment;
import javax.annotation.processing.SupportedAnnotationTypes;
import javax.annotation.processing.SupportedSourceVersion;
import javax.lang.model.SourceVersion;
import javax.lang.model.element.AnnotationMirror;
import javax.lang.model.element.Element;
import javax.lang.model.element.ElementKind;
import javax.lang.model.element.ExecutableElement;
import javax.lang.model.element.Modifier;
import javax.lang.model.element.PackageElement;
import javax.lang.model.element.TypeElement;
import javax.lang.model.element.VariableElement;
import javax.lang.model.type.ArrayType;
import javax.lang.model.type.DeclaredType;
import javax.lang.model.type.PrimitiveType;
import javax.lang.model.type.TypeKind;
import javax.lang.model.type.TypeMirror;
import javax.tools.Diagnostic;
import javax.tools.JavaFileObject;

@SupportedAnnotationTypes({"io.velox.udf.VeloxScalar", "io.velox.udf.VeloxAggregate"})
@SupportedSourceVersion(SourceVersion.RELEASE_8)
public final class VeloxUdfProcessor extends AbstractProcessor {
  private final Set<String> generated = new HashSet<String>();

  @Override
  public boolean process(
      Set<? extends TypeElement> annotations,
      RoundEnvironment roundEnvironment) {
    for (Element element : roundEnvironment.getElementsAnnotatedWith(VeloxScalar.class)) {
      generateScalar((TypeElement) element);
    }
    for (Element element : roundEnvironment.getElementsAnnotatedWith(VeloxAggregate.class)) {
      generateAggregate((TypeElement) element);
    }
    return true;
  }

  private void generateScalar(TypeElement type) {
    String qualified = type.getQualifiedName().toString();
    if (!generated.add(qualified)) {
      return;
    }
    try {
      validateClass(type);
      ExecutableElement evaluate = oneMethod(type, "evaluate");
      JavaType output = javaType(evaluate.getReturnType(), evaluate);
      List<JavaType> inputs = new ArrayList<JavaType>();
      for (VariableElement parameter : evaluate.getParameters()) {
        inputs.add(javaType(parameter.asType(), parameter));
      }
      boolean fallible = throwsType(evaluate, "io.velox.udf.VeloxRowException");

      String packageName = packageName(type);
      String simpleName = type.getSimpleName() + "__VeloxAdapter";
      StringBuilder source = new StringBuilder();
      packageLine(source, packageName);
      source.append("public final class ").append(simpleName)
          .append(" implements io.velox.udf.runtime.BatchScalarAdapter {\n")
          .append("  private final ").append(qualified).append(" function = new ")
          .append(qualified).append("();\n")
          .append("  @Override public byte[] apply(byte[] inputIpc) throws Exception {\n")
          .append("    try (io.velox.udf.runtime.ArrowSupport.InputBatch batch = ")
          .append("io.velox.udf.runtime.ArrowSupport.decode(inputIpc)) {\n")
          .append("      Object[] values = new Object[batch.getRowCount()];\n")
          .append(fallible
              ? "      String[] errors = new String[batch.getRowCount()];\n"
              : "      String[] errors = null;\n")
          .append("      for (int row = 0; row < batch.getRowCount(); ++row) {\n");
      if (fallible) {
        source.append("        try {\n          values[row] = function.evaluate(");
      } else {
        source.append("        values[row] = function.evaluate(");
      }
      appendReadArguments(source, inputs, 0);
      source.append(");\n");
      if (fallible) {
        source.append("        } catch (io.velox.udf.VeloxRowException error) {\n")
            .append("          values[row] = null;\n")
            .append("          errors[row] = error.getMessage();\n")
            .append("        }\n");
      }
      source.append("      }\n")
          .append("      return io.velox.udf.runtime.ArrowSupport.encodeValues(")
          .append(output.spec).append(", values, errors);\n")
          .append("    }\n")
          .append("  }\n")
          .append("  @Override public void close() {}\n")
          .append("}\n");
      write(type, packageName, simpleName, source.toString());
    } catch (ProcessingFailure failure) {
      error(type, failure.getMessage());
    }
  }

  private void generateAggregate(TypeElement type) {
    String qualified = type.getQualifiedName().toString();
    if (!generated.add(qualified)) {
      return;
    }
    try {
      validateClass(type);
      ExecutableElement create = oneMethod(type, "create");
      ExecutableElement destroy = oneMethod(type, "destroy");
      ExecutableElement update = oneMethod(type, "update");
      ExecutableElement serialize = oneMethod(type, "serialize");
      ExecutableElement merge = oneMethod(type, "merge");
      ExecutableElement finish = oneMethod(type, "finish");
      String stateType = create.getReturnType().toString();
      requireParameterType(destroy, 0, stateType);
      requireParameterType(update, 0, stateType);
      requireParameterType(serialize, 0, stateType);
      requireParameterType(merge, 0, stateType);
      requireParameterType(finish, 0, stateType);
      requireParameterCount(create, 0);
      requireParameterCount(destroy, 1);
      requireParameterCount(serialize, 1);
      requireParameterCount(merge, 2);
      requireParameterCount(finish, 1);
      if (!isByteArray(serialize.getReturnType())) {
        throw new ProcessingFailure("serialize must return byte[]");
      }
      if (!isByteArray(merge.getParameters().get(1).asType())) {
        throw new ProcessingFailure("merge intermediate parameter must be byte[]");
      }
      List<JavaType> inputs = new ArrayList<JavaType>();
      for (int i = 1; i < update.getParameters().size(); ++i) {
        VariableElement parameter = update.getParameters().get(i);
        inputs.add(javaType(parameter.asType(), parameter));
      }
      JavaType output = javaType(finish.getReturnType(), finish);

      String packageName = packageName(type);
      String simpleName = type.getSimpleName() + "__VeloxAdapter";
      StringBuilder source = new StringBuilder();
      packageLine(source, packageName);
      source.append("public final class ").append(simpleName)
          .append(" extends io.velox.udf.runtime.GeneratedAggregateAdapter<")
          .append(stateType).append("> {\n")
          .append("  private final ").append(qualified).append(" function = new ")
          .append(qualified).append("();\n")
          .append("  @Override protected ").append(stateType)
          .append(" createState() throws Exception { return function.create(); }\n")
          .append("  @Override protected void destroyState(").append(stateType)
          .append(" state) throws Exception { function.destroy(state); }\n")
          .append("  @Override protected void updateState(").append(stateType)
          .append(" state, io.velox.udf.runtime.ArrowSupport.InputBatch batch, ")
          .append("int row, int argumentOffset) throws Exception {\n")
          .append("    function.update(state");
      for (int i = 0; i < inputs.size(); ++i) {
        source.append(", ").append(inputs.get(i).read("argumentOffset + " + i));
      }
      source.append(");\n  }\n")
          .append("  @Override protected byte[] serializeState(").append(stateType)
          .append(" state) throws Exception { return function.serialize(state); }\n")
          .append("  @Override protected void mergeState(").append(stateType)
          .append(" state, byte[] intermediate) throws Exception {")
          .append(" function.merge(state, intermediate); }\n")
          .append("  @Override protected Object finishState(").append(stateType)
          .append(" state) throws Exception { return function.finish(state); }\n")
          .append("  @Override protected io.velox.udf.runtime.TypeSpec finishType() {")
          .append(" return ").append(output.spec).append("; }\n")
          .append("}\n");
      write(type, packageName, simpleName, source.toString());
    } catch (ProcessingFailure failure) {
      error(type, failure.getMessage());
    }
  }

  private void appendReadArguments(
      StringBuilder source, List<JavaType> inputs, int offset) {
    for (int i = 0; i < inputs.size(); ++i) {
      if (i != 0) {
        source.append(", ");
      }
      source.append(inputs.get(i).read(Integer.toString(offset + i)));
    }
  }

  private JavaType javaType(TypeMirror mirror, Element annotations) {
    boolean nullable = hasAnnotation(annotations, "io.velox.udf.Nullable")
        || hasAnnotation(mirror, "io.velox.udf.Nullable")
        // Some JDK 8 javac builds omit TYPE_USE annotations from
        // TypeMirror#getAnnotationMirrors while retaining them in toString().
        || mirror.toString().contains("@io.velox.udf.Nullable");
    TypeKind kind = mirror.getKind();
    if (kind.isPrimitive()) {
      switch (kind) {
        case BOOLEAN:
          return JavaType.primitive("BOOLEAN", "java.lang.Boolean", "booleanValue");
        case BYTE:
          return JavaType.primitive("TINYINT", "java.lang.Byte", "byteValue");
        case SHORT:
          return JavaType.primitive("SMALLINT", "java.lang.Short", "shortValue");
        case INT:
          return JavaType.primitive("INTEGER", "java.lang.Integer", "intValue");
        case LONG:
          return JavaType.primitive("BIGINT", "java.lang.Long", "longValue");
        case FLOAT:
          return JavaType.primitive("REAL", "java.lang.Float", "floatValue");
        case DOUBLE:
          return JavaType.primitive("DOUBLE", "java.lang.Double", "doubleValue");
        default:
          throw new ProcessingFailure("unsupported primitive UDF type " + mirror);
      }
    }
    String name = mirror.getKind() == TypeKind.DECLARED
        ? ((TypeElement) ((DeclaredType) mirror).asElement())
            .getQualifiedName().toString()
        : processingEnv.getTypeUtils().erasure(mirror).toString();
    if (name.equals("java.lang.Boolean")) {
      return JavaType.reference("BOOLEAN", name, true);
    }
    if (name.equals("java.lang.Byte")) {
      return JavaType.reference("TINYINT", name, true);
    }
    if (name.equals("java.lang.Short")) {
      return JavaType.reference("SMALLINT", name, true);
    }
    if (name.equals("java.lang.Integer")) {
      return JavaType.reference("INTEGER", name, true);
    }
    if (name.equals("java.lang.Long")) {
      return JavaType.reference("BIGINT", name, true);
    }
    if (name.equals("java.lang.Float")) {
      return JavaType.reference("REAL", name, true);
    }
    if (name.equals("java.lang.Double")) {
      return JavaType.reference("DOUBLE", name, true);
    }
    if (name.equals("java.lang.String")) {
      return JavaType.reference("VARCHAR", name, nullable);
    }
    if (isByteArray(mirror)) {
      return JavaType.reference("VARBINARY", "byte[]", nullable);
    }
    if (name.equals("io.velox.udf.VeloxDate32")) {
      return JavaType.reference("DATE", name, nullable);
    }
    if (name.equals("io.velox.udf.VeloxTimestamp")) {
      return JavaType.reference("TIMESTAMP", name, nullable);
    }
    if (name.equals("java.math.BigDecimal")) {
      int[] decimal = decimalParameters(annotations, mirror);
      String spec = "io.velox.udf.runtime.TypeSpec.decimal("
          + decimal[0] + ", " + decimal[1] + ")";
      if (nullable) {
        spec += ".nullable()";
      }
      return new JavaType(spec, name, null, nullable);
    }
    if (name.equals("java.util.List")) {
      DeclaredType declared = (DeclaredType) mirror;
      if (declared.getTypeArguments().size() != 1) {
        throw new ProcessingFailure("List UDF type needs one type argument");
      }
      JavaType element = javaType(declared.getTypeArguments().get(0), null);
      String spec = "io.velox.udf.runtime.TypeSpec.array(" + element.spec + ")";
      if (nullable) {
        spec += ".nullable()";
      }
      return new JavaType(spec, "java.util.List", null, nullable);
    }
    if (name.equals("io.velox.udf.VeloxMap")) {
      DeclaredType declared = (DeclaredType) mirror;
      if (declared.getTypeArguments().size() != 2) {
        throw new ProcessingFailure("VeloxMap UDF type needs two type arguments");
      }
      JavaType key = javaType(declared.getTypeArguments().get(0), null);
      JavaType value = javaType(declared.getTypeArguments().get(1), null);
      if (key.nullable) {
        throw new ProcessingFailure("VeloxMap key type must be non-nullable");
      }
      String spec = "io.velox.udf.runtime.TypeSpec.map("
          + key.spec + ", " + value.spec + ")";
      if (nullable) {
        spec += ".nullable()";
      }
      return new JavaType(spec, "io.velox.udf.VeloxMap", null, nullable);
    }
    if (mirror.getKind() == TypeKind.DECLARED) {
      TypeElement declaredElement =
          (TypeElement) ((DeclaredType) mirror).asElement();
      if (hasAnnotation(declaredElement, "io.velox.udf.VeloxRowType")) {
        return rowType(declaredElement, nullable);
      }
    }
    throw new ProcessingFailure("unsupported Java UDF type " + mirror);
  }

  private JavaType rowType(TypeElement row, boolean nullable) {
    if (!row.getModifiers().contains(Modifier.PUBLIC)) {
      throw new ProcessingFailure("@VeloxRowType class must be public");
    }
    List<VariableElement> ordered = new ArrayList<VariableElement>();
    for (Element enclosed : row.getEnclosedElements()) {
      if (enclosed.getKind() != ElementKind.FIELD) {
        continue;
      }
      io.velox.udf.VeloxField annotation =
          enclosed.getAnnotation(io.velox.udf.VeloxField.class);
      if (annotation == null) {
        continue;
      }
      VariableElement field = (VariableElement) enclosed;
      if (!field.getModifiers().contains(Modifier.PUBLIC)
          || field.getModifiers().contains(Modifier.STATIC)
          || field.getModifiers().contains(Modifier.FINAL)) {
        throw new ProcessingFailure(
            "@VeloxField must be public, non-static, and non-final");
      }
      int index = annotation.index();
      if (index < 0) {
        throw new ProcessingFailure("@VeloxField index must be non-negative");
      }
      while (ordered.size() <= index) {
        ordered.add(null);
      }
      if (ordered.get(index) != null) {
        throw new ProcessingFailure("duplicate @VeloxField index " + index);
      }
      ordered.set(index, field);
    }
    if (ordered.isEmpty()) {
      throw new ProcessingFailure("@VeloxRowType has no @VeloxField fields");
    }
    StringBuilder names = new StringBuilder("new String[] {");
    StringBuilder types = new StringBuilder(
        "new io.velox.udf.runtime.TypeSpec[] {");
    for (int index = 0; index < ordered.size(); ++index) {
      VariableElement field = ordered.get(index);
      if (field == null) {
        throw new ProcessingFailure("missing @VeloxField index " + index);
      }
      io.velox.udf.VeloxField annotation =
          field.getAnnotation(io.velox.udf.VeloxField.class);
      if (annotation.name().isEmpty()) {
        throw new ProcessingFailure("@VeloxField name must not be empty");
      }
      if (index != 0) {
        names.append(", ");
        types.append(", ");
      }
      names.append(javaString(annotation.name()));
      types.append(javaType(field.asType(), field).spec);
    }
    names.append("}");
    types.append("}");
    String qualified = row.getQualifiedName().toString();
    String spec = "io.velox.udf.runtime.TypeSpec.row("
        + qualified + ".class, " + names + ", " + types + ")";
    if (nullable) {
      spec += ".nullable()";
    }
    return new JavaType(spec, qualified, null, nullable);
  }

  private String javaString(String value) {
    return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
  }

  private int[] decimalParameters(Element element, TypeMirror mirror) {
    for (AnnotationMirror annotation : element.getAnnotationMirrors()) {
      if (annotation.getAnnotationType().toString().equals("io.velox.udf.VeloxDecimal")) {
        String text = annotation.toString();
        return parseDecimalAnnotation(text);
      }
    }
    for (AnnotationMirror annotation : mirror.getAnnotationMirrors()) {
      if (annotation.getAnnotationType().toString().equals("io.velox.udf.VeloxDecimal")) {
        return parseDecimalAnnotation(annotation.toString());
      }
    }
    throw new ProcessingFailure("BigDecimal requires @VeloxDecimal(precision, scale)");
  }

  private int[] parseDecimalAnnotation(String text) {
    java.util.regex.Matcher precision =
        java.util.regex.Pattern.compile("precision\\s*=\\s*(\\d+)").matcher(text);
    java.util.regex.Matcher scale =
        java.util.regex.Pattern.compile("scale\\s*=\\s*(\\d+)").matcher(text);
    if (!precision.find() || !scale.find()) {
      throw new ProcessingFailure("invalid @VeloxDecimal annotation");
    }
    return new int[] {
      Integer.parseInt(precision.group(1)), Integer.parseInt(scale.group(1))
    };
  }

  private boolean hasAnnotation(Element element, String name) {
    if (element == null) {
      return false;
    }
    for (AnnotationMirror annotation : element.getAnnotationMirrors()) {
      if (annotation.getAnnotationType().toString().equals(name)) {
        return true;
      }
    }
    return false;
  }

  private boolean hasAnnotation(TypeMirror mirror, String name) {
    if (name.equals("io.velox.udf.Nullable")
        && mirror.getAnnotation(io.velox.udf.Nullable.class) != null) {
      return true;
    }
    for (AnnotationMirror annotation : mirror.getAnnotationMirrors()) {
      if (annotation.getAnnotationType().toString().equals(name)) {
        return true;
      }
    }
    return false;
  }

  private void validateClass(TypeElement type) {
    if (!type.getModifiers().contains(Modifier.PUBLIC)) {
      throw new ProcessingFailure("Java UDF class must be public");
    }
    if (type.getNestingKind().isNested()) {
      throw new ProcessingFailure("nested Java UDF classes are not supported");
    }
  }

  private ExecutableElement oneMethod(TypeElement type, String name) {
    ExecutableElement result = null;
    for (Element element : type.getEnclosedElements()) {
      if (element.getKind() == ElementKind.METHOD
          && element.getSimpleName().contentEquals(name)) {
        if (result != null) {
          throw new ProcessingFailure("Java UDF has multiple " + name + " methods");
        }
        result = (ExecutableElement) element;
      }
    }
    if (result == null) {
      throw new ProcessingFailure("Java UDF is missing " + name + " method");
    }
    if (result.getModifiers().contains(Modifier.STATIC)
        || !result.getModifiers().contains(Modifier.PUBLIC)) {
      throw new ProcessingFailure(name + " must be a public instance method");
    }
    return result;
  }

  private void requireParameterCount(ExecutableElement method, int count) {
    if (method.getParameters().size() != count) {
      throw new ProcessingFailure(
          method.getSimpleName() + " must have " + count + " parameters");
    }
  }

  private void requireParameterType(
      ExecutableElement method, int index, String expected) {
    if (method.getParameters().size() <= index
        || !method.getParameters().get(index).asType().toString().equals(expected)) {
      throw new ProcessingFailure(
          method.getSimpleName() + " parameter " + index + " must be " + expected);
    }
  }

  private boolean throwsType(ExecutableElement method, String name) {
    for (TypeMirror thrown : method.getThrownTypes()) {
      if (thrown.toString().equals(name)) {
        return true;
      }
    }
    return false;
  }

  private boolean isByteArray(TypeMirror mirror) {
    return mirror.getKind() == TypeKind.ARRAY
        && ((ArrayType) mirror).getComponentType().getKind() == TypeKind.BYTE;
  }

  private String packageName(TypeElement type) {
    return ((PackageElement) type.getEnclosingElement()).getQualifiedName().toString();
  }

  private void packageLine(StringBuilder source, String packageName) {
    if (!packageName.isEmpty()) {
      source.append("package ").append(packageName).append(";\n\n");
    }
  }

  private void write(
      TypeElement origin,
      String packageName,
      String simpleName,
      String source) {
    String qualified = packageName.isEmpty() ? simpleName : packageName + "." + simpleName;
    try {
      JavaFileObject file = processingEnv.getFiler().createSourceFile(qualified, origin);
      Writer writer = file.openWriter();
      try {
        writer.write(source);
      } finally {
        writer.close();
      }
    } catch (IOException error) {
      throw new ProcessingFailure("cannot generate adapter: " + error.getMessage());
    }
  }

  private void error(Element element, String message) {
    processingEnv.getMessager().printMessage(Diagnostic.Kind.ERROR, message, element);
  }

  private static final class JavaType {
    final String spec;
    final String castType;
    final String unboxMethod;
    final boolean nullable;

    JavaType(String spec, String castType, String unboxMethod) {
      this(spec, castType, unboxMethod, false);
    }

    JavaType(
        String spec, String castType, String unboxMethod, boolean nullable) {
      this.spec = spec;
      this.castType = castType;
      this.unboxMethod = unboxMethod;
      this.nullable = nullable;
    }

    static JavaType primitive(String kind, String wrapper, String unboxMethod) {
      return new JavaType(
          "io.velox.udf.runtime.TypeSpec." + kind, wrapper, unboxMethod, false);
    }

    static JavaType reference(String kind, String castType, boolean nullable) {
      String spec = "io.velox.udf.runtime.TypeSpec." + kind;
      if (nullable) {
        spec += ".nullable()";
      }
      return new JavaType(spec, castType, null, nullable);
    }

    String read(String column) {
      String value = "((" + castType + ") io.velox.udf.runtime.ArrowSupport.read("
          + "batch, " + column + ", row, " + spec + "))";
      return unboxMethod == null ? value : value + "." + unboxMethod + "()";
    }
  }

  private static final class ProcessingFailure extends RuntimeException {
    ProcessingFailure(String message) {
      super(message);
    }
  }
}
