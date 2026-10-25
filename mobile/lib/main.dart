import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'core/node_controller.dart';
import 'ui/home_screen.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final controller = NodeController();
  await controller.initialize();
  runApp(
    ChangeNotifierProvider<NodeController>.value(
      value: controller,
      child: const BenshiNodeApp(),
    ),
  );
}

class BenshiNodeApp extends StatelessWidget {
  const BenshiNodeApp({super.key});

  @override
  Widget build(BuildContext context) {
    final scheme = ColorScheme.fromSeed(
      seedColor: const Color(0xFF33D17A),
      brightness: Brightness.dark,
    );
    return MaterialApp(
      title: 'BenshiNode',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: scheme,
        useMaterial3: true,
        scaffoldBackgroundColor: const Color(0xFF101512),
        cardTheme: CardThemeData(
          color: const Color(0xFF18211C),
          elevation: 0,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(14),
            side: const BorderSide(color: Color(0xFF2A3830)),
          ),
        ),
        inputDecorationTheme: InputDecorationTheme(
          filled: true,
          fillColor: const Color(0xFF121A15),
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(10),
          ),
        ),
      ),
      home: const HomeScreen(),
    );
  }
}
