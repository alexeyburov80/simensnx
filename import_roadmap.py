#!/usr/bin/env python3
import re
import subprocess
import time
import sys

REPO = "alexeyburov80/simensnx"
PROJECT_NUMBER = "3"

def run_command(cmd):
    """Запускает команду и возвращает вывод"""
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except Exception as e:
        return False, "", str(e)

def create_issue(title, body, labels):
    """Создает issue через gh CLI"""
    cmd = f'gh issue create --repo {REPO} --title "{title}" --body "{body}" --label "{labels}"'
    success, stdout, stderr = run_command(cmd)
    return success, stdout, stderr

def add_to_project(issue_num):
    """Добавляет issue в проект"""
    cmd = f'gh project item-add {PROJECT_NUMBER} --owner {REPO} --url "https://github.com/{REPO}/issues/{issue_num}"'
    success, stdout, stderr = run_command(cmd)
    return success, stdout, stderr

def parse_roadmap(filepath):
    """Парсит ROADMAP.md"""
    tasks = []
    current_phase = ""
    
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            # Проверяем фазу
            phase_match = re.match(r'^## Phase (\d+)', line.strip())
            if phase_match:
                current_phase = line.strip().replace('## ', '')
                continue
            
            # Проверяем задачу
            task_match = re.match(r'^\s*[-*]\s*\[([ x])\]\s*(.+)$', line.strip())
            if task_match:
                status = task_match.group(1)
                title = task_match.group(2).strip()
                tasks.append({
                    'phase': current_phase,
                    'phase_num': re.search(r'\d+', current_phase).group(0) if re.search(r'\d+', current_phase) else '0',
                    'title': title,
                    'completed': status == 'x'
                })
    
    return tasks

def main():
    print("🚀 Начинаем импорт в проект: @alexeyburov80-simensnx")
    print("📖 Парсинг ROADMAP.md...")
    
    tasks = parse_roadmap('ROADMAP.md')
    print(f"📊 Найдено {len(tasks)} задач\n")
    
    total = len(tasks)
    created = 0
    added = 0
    
    for i, task in enumerate(tasks, 1):
        print(f"[{i}/{total}] 📝 {task['title']}")
        
        # Формируем тело issue
        status = "✅ Выполнено" if task['completed'] else "⏳ В работе/ожидает"
        body = f"""**Фаза:** {task['phase']}
**Статус:** {status}

**Описание:** {task['title']}

**Контекст:**
- Исходный пункт из ROADMAP.md
- [ ] Реализовать

---
*Автоматически импортировано из ROADMAP.md*"""
        
        title = f"[Phase {task['phase_num']}] {task['title']}"
        labels = f"roadmap,phase-{task['phase_num']}"
        
        # Создаем issue
        print("  ➜ Создаю issue...")
        success, stdout, stderr = create_issue(title, body, labels)
        
        if success and stdout:
            print(f"  ✅ Создано: {stdout}")
            created += 1
            
            # Извлекаем номер issue
            issue_num = re.search(r'\d+$', stdout).group(0) if re.search(r'\d+$', stdout) else None
            
            if issue_num:
                print("  ➜ Добавляю в проект...")
                success2, stdout2, stderr2 = add_to_project(issue_num)
                if success2:
                    print("  ✅ Добавлено в проект @alexeyburov80-simensnx")
                    added += 1
                else:
                    print(f"  ⚠️  Не удалось добавить в проект: {stderr2}")
        else:
            print(f"  ❌ Ошибка: {stderr}")
        
        print()
        time.sleep(1)
    
    print("=" * 50)
    print(f"📊 Итог импорта:")
    print(f"   Всего задач: {total}")
    print(f"   Создано issues: {created}")
    print(f"   Добавлено в проект: {added}")
    print("=" * 50)
    print("✅ Готово!")

if __name__ == "__main__":
    main()