/* extern в заголовке и inline в этом файле — одно внешнее определение. */
extern int ext_inline_add(int a, int b);

inline int ext_inline_add(int a, int b) { return a + b; }
